#include "processing_cache.hpp"
#include "paths.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include <utility>
#include <windows.h>

namespace ve {
namespace {
constexpr auto markerName = ".ve-processing-cache.json";
constexpr auto liveName = ".ve-processing-live.lock";
constexpr auto formatName = "visual-enhancer-processing-cache";
QString normalized(QString path) {
    if (path.startsWith("\\\\?\\")) path.remove(0, 4);
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}
QString metadataDirectory() {
    const auto path = QDir(Paths::instance().cacheDirectory()).filePath("processing");
    if (!QDir().mkpath(path + "/sessions"))
        throw std::runtime_error("Cannot create processing cache metadata.");
    return path;
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = std::exchange(other.value, INVALID_HANDLE_VALUE); return *this;
    }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
    Handle(const Handle&) = delete;
};
Handle openLock(const QString& path, bool exclusive, bool wait = false, bool create = true) {
    Handle handle(CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return {};
    OVERLAPPED overlapped{};
    if (!LockFileEx(handle.value, (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) |
                        (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY), 0, 1, 0, &overlapped)) return {};
    return handle;
}
QString finalPath(HANDLE handle) {
    wchar_t buffer[32768]{};
    const auto length = GetFinalPathNameByHandleW(handle, buffer, DWORD(std::size(buffer)), FILE_NAME_NORMALIZED);
    return length && length < std::size(buffer) ? normalized(QString::fromWCharArray(buffer, int(length))) : QString{};
}
QString fileIdentity(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return {};
    return QString("%1:%2:%3:%4:%5").arg(info.dwVolumeSerialNumber, 8, 16, QChar('0'))
        .arg((quint64(info.nFileIndexHigh) << 32) | info.nFileIndexLow, 16, 16, QChar('0'))
        .arg((quint64(info.ftCreationTime.dwHighDateTime) << 32) | info.ftCreationTime.dwLowDateTime, 16, 16, QChar('0'))
        .arg((quint64(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime, 16, 16, QChar('0'))
        .arg((quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow, 16, 16, QChar('0'));
}
QString fileIdentity(const QString& path) {
    Handle file(CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    return file ? fileIdentity(file.value) : QString{};
}
bool relativeName(const QString& relative) {
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains('\\') || relative.contains(':'))
        return false;
    for (const auto character : relative)
        if (character.unicode() < 32 || QStringView(u"<>\"|?*").contains(character)) return false;
    for (const auto& part : relative.split('/'))
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) return false;
    return true;
}
// Deny rename/delete on every directory from the trusted boundary to the file.
// Delete the checked file handle, never a path re-resolved after validation.
bool secureRemove(const QString& boundary, const QString& root, const QString& relative,
                  const QString& identity = {}, const QByteArray& digest = {}) {
    if (!relativeName(relative)) return false;
    const auto base = normalized(QFileInfo(boundary).absoluteFilePath());
    const auto owner = normalized(QFileInfo(root).absoluteFilePath());
    if (owner != base && !owner.startsWith(base + '/')) return false;
    const auto target = QDir::cleanPath(root + '/' + relative);
    const auto parent = normalized(QFileInfo(target).absolutePath());
    if (parent != owner && !parent.startsWith(owner + '/')) return false;
    std::vector<Handle> pins;
    QString resolvedBase;
    auto pin = [&](const QString& path) {
        Handle h(CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
            FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!h || !GetFileInformationByHandle(h.value, &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        const auto resolved = finalPath(h.value);
        if (resolved.isEmpty()) return false;
        // Windows can transparently map a standard AppData location for the
        // caller's package identity. Anchor to that opened standard directory;
        // every descendant must remain below the same resolved boundary.
        if (resolvedBase.isEmpty()) resolvedBase = resolved;
        else if (resolved != resolvedBase + normalized(path).mid(base.size())) return false;
        pins.push_back(std::move(h)); return true;
    };
    if (!pin(boundary)) return false;
    auto current = base;
    const auto tail = parent.mid(base.size());
    for (const auto& part : tail.split('/', Qt::SkipEmptyParts)) {
        current += '/' + part;
        if (!pin(current)) return false;
    }
    Handle file(CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
        DELETE | FILE_READ_ATTRIBUTES | (digest.isEmpty() ? 0 : GENERIC_READ), FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!file || !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        finalPath(file.value) != resolvedBase + normalized(target).mid(base.size())) return false;
    if (!identity.isEmpty() && fileIdentity(file.value) != identity) return false;
    if (!digest.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        char buffer[65536]; DWORD read = 0; qint64 total = 0;
        do {
            if (!ReadFile(file.value, buffer, sizeof(buffer), &read, nullptr)) return false;
            total += read;
            if (total > 1024 * 1024) return false;
            hash.addData(QByteArrayView(buffer, read));
        } while (read);
        if (hash.result().toHex() != digest) return false;
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
}
bool safeFile(const QString& boundary, const QString& path) {
    const auto base = normalized(boundary), file = normalized(path);
    if (!file.startsWith(base + '/')) return false;
    auto current = QFileInfo(path).absoluteFilePath();
    for (;;) {
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (normalized(current) == base) break;
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current || !normalized(parent).startsWith(base)) return false;
        current = parent;
    }
    return QFileInfo(path).isFile();
}
bool saveJson(const QString& path, const QJsonObject& object) {
    QSaveFile file(path);
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
QJsonObject readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
QMutex rootsMutex;
QHash<QString, std::weak_ptr<ProcessingCacheRoot::State>> roots;
} // namespace

struct ProcessingCacheRoot::State {
    mutable QMutex mutex;
    QString id, root, index;
    ProcessingCacheKind kind;
    QSet<QString> entries, protectedPaths;
    QHash<QString, int> readers;
    QHash<QString, QString> identities;
    Handle live;
    QJsonObject object() const {
        QJsonArray files;
        auto sorted = entries.values(); std::ranges::sort(sorted);
        for (const auto& file : sorted) files.append(file);
        QJsonObject ids;
        for (auto it = identities.begin(); it != identities.end(); ++it) ids.insert(it.key(), it.value());
        return {{"format", formatName}, {"schema", 1}, {"id", id}, {"root", root},
                {"kind", int(kind)}, {"files", files}, {"identities", ids}};
    }
    void persist() const {
        if (!saveJson(root + '/' + markerName, object()))
            throw std::runtime_error("Cannot record processing cache ownership.");
    }
};
namespace {
struct Record {
    QString id, root, index;
    ProcessingCacheKind kind{};
    QSet<QString> entries, protectedPaths;
    bool current = false, live = false;
    QJsonObject marker;
    QHash<QString, QString> identities;
};
std::vector<Record> records(int& skipped) {
    std::vector<Record> result;
    QSet<QString> seen;
    {
        QMutexLocker all(&rootsMutex);
        for (auto it = roots.begin(); it != roots.end(); ++it) {
            const auto state = it.value().lock();
            if (!state) continue;
            QMutexLocker lock(&state->mutex);
            result.push_back({state->id, state->root, state->index, state->kind, state->entries,
                              state->protectedPaths, true, true});
            for (auto reader = state->readers.begin(); reader != state->readers.end(); ++reader)
                if (reader.value() > 0) result.back().protectedPaths.insert(reader.key());
            result.back().identities = state->identities;
            seen.insert(state->id);
        }
    }
    const QDir directory(metadataDirectory() + "/sessions");
    const QRegularExpression idPattern("^[a-f0-9]{32}$");
    for (const auto& name : directory.entryList({"*.json"}, QDir::Files | QDir::NoSymLinks)) {
        const auto id = QFileInfo(name).completeBaseName();
        if (seen.contains(id)) continue;
        if (!idPattern.match(id).hasMatch()) { ++skipped; continue; }
        const auto index = directory.filePath(name);
        if (!safeFile(metadataDirectory(), index)) { ++skipped; continue; }
        const auto locator = readJson(index);
        const auto root = locator.value("root").toString();
        const auto temp = normalized(QDir::tempPath());
        if (locator.value("format") != formatName || locator.value("id") != id ||
            !normalized(root).startsWith(temp + '/')) { ++skipped; continue; }
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) continue;
        if (!QFileInfo(root).isDir() ||
            !safeFile(QDir::tempPath(), root + '/' + markerName)) { ++skipped; continue; }
        const auto marker = readJson(root + '/' + markerName);
        const auto kind = marker.value("kind").toInt(-1);
        if (marker.value("format") != formatName || marker.value("schema").toInt() != 1 ||
            marker.value("id") != id || normalized(marker.value("root").toString()) != normalized(root) ||
            kind < 0 || kind > int(ProcessingCacheKind::Scratch) || !marker.value("files").isArray()) {
            ++skipped; continue;
        }
        Record record{id, root, index, ProcessingCacheKind(kind)};
        record.marker = marker;
        const auto ids = marker.value("identities").toObject();
        for (auto it = ids.begin(); it != ids.end(); ++it)
            if (it.value().isString()) record.identities.insert(it.key(), it.value().toString());
        bool valid = true;
        for (const auto& value : marker.value("files").toArray()) {
            if (!value.isString() || !relativeName(value.toString()) ||
                value.toString() == markerName || value.toString() == liveName) { valid = false; break; }
            record.entries.insert(value.toString());
        }
        if (!valid) { ++skipped; continue; }
        auto lock = openLock(root + '/' + liveName, true, false, false);
        record.live = !lock;
        result.push_back(std::move(record));
    }
    return result;
}
struct Hook { QString path; qint64 bytes; };
void pruneMissingSessionIndexes() {
    const QDir sessions(metadataDirectory() + "/sessions");
    const QRegularExpression pattern("^[a-f0-9]{32}\\.json$");
    for (const auto& name : sessions.entryList({"*.json"}, QDir::Files | QDir::NoSymLinks)) {
        if (!pattern.match(name).hasMatch()) continue;
        const auto path = sessions.filePath(name);
        if (!safeFile(metadataDirectory(), path)) continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) continue;
        const auto data = file.readAll(); file.close();
        const auto locator = QJsonDocument::fromJson(data).object();
        const auto root = locator.value("root").toString();
        if (locator.size() != 3 || locator.value("format") != formatName ||
            locator.value("id") != QFileInfo(name).completeBaseName() ||
            !normalized(root).startsWith(normalized(QDir::tempPath()) + '/')) continue;
        const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root).utf16()));
        const auto error = GetLastError();
        if (attributes == INVALID_FILE_ATTRIBUTES && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
            secureRemove(Paths::instance().cacheDirectory(), sessions.path(), name, {},
                         QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    }
}
std::vector<Hook> hooks(int& skipped) {
    const auto base = Paths::instance().cacheDirectory();
    const QDir directory(base + "/shaders");
    std::vector<Hook> result;
    const QRegularExpression pattern("^shader-([a-f0-9]{64})\\.hook$");
    for (const auto& info : directory.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
        const auto match = pattern.match(info.fileName());
        if (!match.hasMatch()) continue;
        if (!safeFile(base, info.absoluteFilePath()) || info.size() > 1024 * 1024) { ++skipped; continue; }
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly) ||
            QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex() != match.captured(1).toLatin1()) {
            ++skipped; continue;
        }
        result.push_back({info.absoluteFilePath(), info.size()});
    }
    return result;
}
} // namespace

void ProcessingCacheCleanup::append(const ProcessingCacheCleanup& other) {
    removedBytes += other.removedBytes; removedFiles += other.removedFiles; skipped += other.skipped;
    removedPaths += other.removedPaths;
}
struct ProcessingCacheActivity::Impl { Handle lease; Impl() : lease(openLock(metadataDirectory() + "/shaders.lock", false, true)) {
    if (!lease) throw std::runtime_error("Cannot acquire processing cache activity lock.");
}};
ProcessingCacheActivity::ProcessingCacheActivity() : impl_(std::make_unique<Impl>()) {}
ProcessingCacheActivity::~ProcessingCacheActivity() = default;
ProcessingCache& ProcessingCache::instance() { static ProcessingCache service; return service; }
void ProcessingCache::changed() { emit usageChanged(); }

ProcessingCacheRoot::ProcessingCacheRoot(std::shared_ptr<State> state) : state_(std::move(state)) {}
std::shared_ptr<ProcessingCacheRoot> ProcessingCacheRoot::create(const QString& path, ProcessingCacheKind kind) {
    auto state = std::make_shared<State>();
    state->id = QUuid::createUuid().toString(QUuid::Id128);
    state->root = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (!normalized(state->root).startsWith(normalized(QDir::tempPath()) + '/'))
        throw std::runtime_error("Processing cache must use the system temporary directory.");
    state->kind = kind;
    state->live = openLock(path + '/' + liveName, true);
    if (!state->live) throw std::runtime_error("Cannot lock processing cache ownership.");
    state->index = metadataDirectory() + "/sessions/" + state->id + ".json";
    state->persist();
    if (!saveJson(state->index, {{"format", formatName}, {"id", state->id}, {"root", state->root}}))
        throw std::runtime_error("Cannot index processing cache ownership.");
    { QMutexLocker lock(&rootsMutex); roots.insert(state->id, state); }
    ProcessingCache::instance().changed();
    return std::shared_ptr<ProcessingCacheRoot>(new ProcessingCacheRoot(std::move(state)));
}
ProcessingCacheRoot::~ProcessingCacheRoot() {
    { QMutexLocker lock(&rootsMutex); roots.remove(state_->id); }
    state_->live = {};
    if (!QFileInfo::exists(state_->root)) QFile::remove(state_->index);
    ProcessingCache::instance().changed();
}
const QString& ProcessingCacheRoot::path() const { return state_->root; }
void ProcessingCacheRoot::registerFile(const QString& path) {
    const auto relative = QDir(state_->root).relativeFilePath(path);
    if (!relativeName(relative) || relative == markerName || relative == liveName)
        throw std::runtime_error("Invalid processing cache file.");
    QMutexLocker lock(&state_->mutex);
    if (state_->entries.contains(relative)) return;
    state_->entries.insert(relative);
    state_->persist();
    ProcessingCache::instance().changed();
}
void ProcessingCacheRoot::protect(const QSet<QString>& paths) {
    QMutexLocker lock(&state_->mutex);
    QSet<QString> updated;
    for (const auto& path : paths) updated.insert(normalized(path));
    if (updated == state_->protectedPaths) return;
    state_->protectedPaths = std::move(updated);
    ProcessingCache::instance().changed();
}
void ProcessingCacheRoot::publishFile(const QString& path) {
    const auto relative = QDir(state_->root).relativeFilePath(path);
    QMutexLocker lock(&state_->mutex);
    if (!state_->entries.contains(relative)) throw std::runtime_error("Unregistered processing cache file.");
    const auto identity = fileIdentity(path);
    if (identity.isEmpty()) throw std::runtime_error("Cannot identify processing cache file.");
    state_->identities.insert(relative, identity);
    state_->persist();
    ProcessingCache::instance().changed();
}
std::shared_ptr<void> ProcessingCacheRoot::leaseFile(const QString& path) {
    const auto key = normalized(path);
    { QMutexLocker lock(&state_->mutex); ++state_->readers[key]; }
    ProcessingCache::instance().changed();
    return std::shared_ptr<void>(new int(0), [state = state_, key](void* pointer) {
        delete static_cast<int*>(pointer);
        { QMutexLocker lock(&state->mutex); if (--state->readers[key] == 0) state->readers.remove(key); }
        ProcessingCache::instance().changed();
    });
}
QStringList ProcessingCacheRoot::files() const {
    QMutexLocker lock(&state_->mutex);
    QStringList files;
    for (const auto& entry : state_->entries) files.append(state_->root + '/' + entry);
    return files;
}
bool ProcessingCacheRoot::removeFile(const QString& path) {
    const auto relative = QDir(state_->root).relativeFilePath(path);
    QMutexLocker lock(&state_->mutex);
    if (!state_->entries.contains(relative) || !relativeName(relative) ||
        state_->protectedPaths.contains(normalized(path)) || state_->readers.value(normalized(path)) > 0) return false;
    const auto attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    } else {
        const auto identity = state_->identities.value(relative);
        if (state_->kind != ProcessingCacheKind::Scratch && identity.isEmpty()) return false;
        if (!secureRemove(QDir::tempPath(), state_->root, relative, identity)) return false;
    }
    state_->entries.remove(relative);
    state_->identities.remove(relative);
    try { state_->persist(); } catch (...) { /* A missing entry is harmless on recovery. */ }
    ProcessingCache::instance().changed();
    return true;
}
ProcessingTemporaryDirectory::ProcessingTemporaryDirectory(ProcessingCacheKind kind) : QTemporaryDir() {
    if (isValid()) owner_ = ProcessingCacheRoot::create(path(), kind);
}
ProcessingTemporaryDirectory::ProcessingTemporaryDirectory(const QString& name, ProcessingCacheKind kind) : QTemporaryDir(name) {
    if (isValid()) owner_ = ProcessingCacheRoot::create(path(), kind);
}
ProcessingTemporaryDirectory::~ProcessingTemporaryDirectory() {
    if (owner_) {
        // QTemporaryDir cannot remove its directory while the no-delete lease
        // is open. Keep the state registered until normal removal has finished.
        owner_->state_->live = {};
        if (autoRemove()) remove();
        owner_.reset();
    }
}
QString ProcessingTemporaryDirectory::filePath(const QString& name) const {
    const auto file = QTemporaryDir::filePath(name);
    if (owner_) owner_->registerFile(file);
    return file;
}
ProcessingCacheSnapshot ProcessingCache::snapshot(const QSet<QString>& protectedPaths) const {
    ProcessingCacheSnapshot result;
    QSet<QString> protectedNames, seen;
    for (const auto& path : protectedPaths) protectedNames.insert(normalized(path));
    auto shaderLock = openLock(metadataDirectory() + "/shaders.lock", true);
    const bool processingActive = !shaderLock;
    shaderLock = {}; // A read-only size scan must not delay a new render.
    for (const auto& record : records(result.skipped)) {
        for (const auto& entry : record.entries) {
            const auto file = record.root + '/' + entry, key = normalized(file);
            if (seen.contains(key) || !safeFile(QDir::tempPath(), file)) continue;
            if (record.kind != ProcessingCacheKind::Scratch &&
                (record.identities.value(entry).isEmpty() || fileIdentity(file) != record.identities.value(entry))) {
                ++result.skipped; continue;
            }
            seen.insert(key);
            const auto bytes = QFileInfo(file).size();
            const bool used = protectedNames.contains(key) || record.protectedPaths.contains(key) ||
                (record.live && (!record.current || record.kind == ProcessingCacheKind::Scratch)) ||
                (!record.live && processingActive);
            result.bytes += bytes;
            (used ? result.inUseBytes : result.reclaimableBytes) += bytes;
        }
    }
    for (const auto& hook : hooks(result.skipped)) {
        const bool used = processingActive || protectedNames.contains(normalized(hook.path));
        result.bytes += hook.bytes;
        (used ? result.inUseBytes : result.reclaimableBytes) += hook.bytes;
    }
    return result;
}
ProcessingCacheCleanup ProcessingCache::cleanAbandonedAndShaders(const QSet<QString>& protectedPaths) {
    ProcessingCacheCleanup result;
    auto maintenance = openLock(metadataDirectory() + "/cleanup.lock", true);
    if (!maintenance) { ++result.skipped; return result; }
    // A child can outlive a failed parent. Its activity lease also protects the
    // parent's abandoned request files until the child has stopped.
    auto shaderLock = openLock(metadataDirectory() + "/shaders.lock", true);
    if (!shaderLock) { ++result.skipped; return result; }
    pruneMissingSessionIndexes();
    QSet<QString> protectedNames;
    for (const auto& path : protectedPaths) protectedNames.insert(normalized(path));
    for (const auto& record : records(result.skipped)) {
        if (record.current || record.live) continue;
        auto owner = openLock(record.root + '/' + liveName, true, false, false);
        if (!owner) { ++result.skipped; continue; }
        // Re-read after acquiring ownership; never use a changed manifest.
        const auto marker = readJson(record.root + '/' + markerName);
        if (marker != record.marker) {
            ++result.skipped; continue;
        }
        for (const auto& entry : record.entries) {
            const auto path = record.root + '/' + entry;
            if (!QFileInfo::exists(path)) continue;
            if (protectedNames.contains(normalized(path)) || !safeFile(QDir::tempPath(), path)) { ++result.skipped; continue; }
            const auto bytes = QFileInfo(path).size();
            const auto identity = record.identities.value(entry);
            if (record.kind != ProcessingCacheKind::Scratch && identity.isEmpty()) { ++result.skipped; continue; }
            if (secureRemove(QDir::tempPath(), record.root, entry, identity)) {
                result.removedBytes += bytes; ++result.removedFiles; result.removedPaths.append(path);
            } else ++result.skipped;
        }
        const auto remaining = QDir(record.root).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
        if (std::ranges::all_of(remaining, [](const QString& name) { return name == markerName || name == liveName; })) {
            owner = {};
            if (secureRemove(QDir::tempPath(), record.root, markerName) &&
                secureRemove(QDir::tempPath(), record.root, liveName)) {
                QDir().rmdir(record.root);
                QFile::remove(record.index);
            }
        }
    }
    const auto shaderHooks = hooks(result.skipped);
    for (const auto& hook : shaderHooks) {
        if (!shaderLock || protectedNames.contains(normalized(hook.path))) { ++result.skipped; continue; }
        if (secureRemove(Paths::instance().cacheDirectory(), Paths::instance().cacheDirectory() + "/shaders",
                         QFileInfo(hook.path).fileName(), {}, QFileInfo(hook.path).completeBaseName().mid(7).toLatin1())) {
            result.removedBytes += hook.bytes; ++result.removedFiles; result.removedPaths.append(hook.path);
        } else ++result.skipped;
    }
    changed();
    return result;
}
} // namespace ve
