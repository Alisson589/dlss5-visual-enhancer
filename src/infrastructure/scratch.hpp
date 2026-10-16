#pragma once
#include <QDir>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include "processing_cache.hpp"
#include <stdexcept>

namespace ve {
namespace scratch_detail {
inline QMutex mutex;
inline QString workerRoot;
inline QString directoryTemplate(const QString& name) {
    QMutexLocker lock(&mutex);
    return QDir(workerRoot.isEmpty() ? QDir::tempPath() : workerRoot).filePath(name);
}
} // namespace scratch_detail

// Ordinary calls use Qt's system temporary directory. An isolated worker's
// nested scratch belongs to the request directory held by its parent process,
// so termination of the child cannot leave its processing files behind.
class ScratchDirectory : public ProcessingTemporaryDirectory {
  public:
    explicit ScratchDirectory(const QString& name)
        : ProcessingTemporaryDirectory(scratch_detail::directoryTemplate(name), ProcessingCacheKind::Scratch) {}
};

class WorkerScratchScope {
  public:
    explicit WorkerScratchScope(const QString& requestDirectory) {
        const auto directory = QDir(requestDirectory).filePath("scratch");
        if (!QDir().mkpath(directory))
            throw std::runtime_error("Cannot create worker scratch storage.");
        QMutexLocker lock(&scratch_detail::mutex);
        previous_ = scratch_detail::workerRoot;
        scratch_detail::workerRoot = directory;
    }
    ~WorkerScratchScope() {
        QMutexLocker lock(&scratch_detail::mutex);
        scratch_detail::workerRoot = previous_;
    }
    WorkerScratchScope(const WorkerScratchScope&) = delete;
    WorkerScratchScope& operator=(const WorkerScratchScope&) = delete;

  private:
    QString previous_;
};
} // namespace ve
