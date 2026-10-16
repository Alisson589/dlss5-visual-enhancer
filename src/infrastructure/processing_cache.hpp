#pragma once
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>
#include <memory>

namespace ve {
enum class ProcessingCacheKind { PreviewFrames, PreviewClips, Scratch };
struct ProcessingCacheSnapshot {
    qint64 bytes = 0, reclaimableBytes = 0, inUseBytes = 0;
    int skipped = 0;
};
struct ProcessingCacheCleanup {
    qint64 removedBytes = 0;
    int removedFiles = 0, skipped = 0;
    QStringList removedPaths;
    void append(const ProcessingCacheCleanup& other);
};

// One shared OS lock covers shader users in the GUI, CLI and VFX worker.
// The lease must enclose graph construction AND its consuming process.
class ProcessingCacheActivity {
  public:
    ProcessingCacheActivity();
    ~ProcessingCacheActivity();
    ProcessingCacheActivity(const ProcessingCacheActivity&) = delete;
    ProcessingCacheActivity& operator=(const ProcessingCacheActivity&) = delete;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ProcessingCacheRoot {
  public:
    struct State;
    static std::shared_ptr<ProcessingCacheRoot> create(const QString& path, ProcessingCacheKind kind);
    ~ProcessingCacheRoot();
    void registerFile(const QString& path);
    void publishFile(const QString& path);
    void protect(const QSet<QString>& paths);
    [[nodiscard]] std::shared_ptr<void> leaseFile(const QString& path);
    [[nodiscard]] QStringList files() const;
    [[nodiscard]] bool removeFile(const QString& path);
    [[nodiscard]] const QString& path() const;
  private:
    friend class ProcessingTemporaryDirectory;
    explicit ProcessingCacheRoot(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

// File paths are recorded before a producer writes them. Unknown siblings are
// never adopted, including after a crash. QTemporaryDir still owns normal exit.
class ProcessingTemporaryDirectory : public QTemporaryDir {
  public:
    explicit ProcessingTemporaryDirectory(ProcessingCacheKind kind);
    ProcessingTemporaryDirectory(const QString& name, ProcessingCacheKind kind);
    ~ProcessingTemporaryDirectory();
    [[nodiscard]] QString filePath(const QString& name) const;
    [[nodiscard]] std::shared_ptr<ProcessingCacheRoot> cacheRoot() const { return owner_; }
  private:
    std::shared_ptr<ProcessingCacheRoot> owner_;
};

class ProcessingCache : public QObject {
    Q_OBJECT
  public:
    static ProcessingCache& instance();
    [[nodiscard]] ProcessingCacheSnapshot snapshot(const QSet<QString>& protectedPaths = {}) const;
    // Live preview roots are cleaned by their owning provider/controller only.
    [[nodiscard]] ProcessingCacheCleanup cleanAbandonedAndShaders(const QSet<QString>& protectedPaths = {});
    void changed();
  signals:
    void usageChanged();
};
} // namespace ve
