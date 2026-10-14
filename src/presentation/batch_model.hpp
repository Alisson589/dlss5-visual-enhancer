#pragma once
#include "media/media.hpp"
#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QTimer>
#include <vector>
#include <memory>

namespace ve {
struct BatchEntry {
    QString input, output, state = "Queued", detail, inputDimensions, outputDimensions, thumbnail;
    double progress = 0, elapsed = 0, duration = 0, processingFps = 0, remaining = 0;
    qint64 bytes = 0, frameCount = 0, processedFrames = 0, totalFrames = 0;
    MediaInfo source, result;
    std::shared_ptr<void> inputOwner; // Optional lifetime of an imported temporary image.
};
class BatchModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedIndexChanged)
  public:
    enum Role {
        FileName = Qt::UserRole + 1,
        InputPath,
        OutputPath,
        ItemState,
        Progress,
        Detail,
        ElapsedSeconds,
        InputDimensions,
        OutputDimensions,
        ThumbnailUrl,
        ItemIndex,
        Selected,
        FileSizeBytes,
        DurationSeconds,
        FrameCount,
        ProcessedFrames,
        ProcessingTotalFrames,
        ProcessingFps,
        RemainingSeconds
    };
    explicit BatchModel(QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const { return int(entries_.size()); }
    [[nodiscard]] int selectedIndex() const { return selected_; }
    [[nodiscard]] QString selectedPath() const;
    [[nodiscard]] QStringList paths() const;
    [[nodiscard]] const BatchEntry& entry(int index) const;
    [[nodiscard]] int indexOf(const QString& path) const;
    void add(const QStringList& paths, std::shared_ptr<void> inputOwner = {});
    void select(int index);
    void remove(int index);
    void clear();
    void clearCompleted();
    void retryFailed();
    void resetStates();
    void update(int row, const QString& state, double fraction, const QString& message);
    void metadata(int row, const MediaInfo& info, const QString& thumbnail = {});
    void completed(int row, const QString& output, const MediaInfo& info);
    void metrics(int row, qint64 processed, qint64 total, double fps);
    void setPaused(bool paused);
  signals:
    void countChanged();
    void selectedIndexChanged();

  private:
    std::vector<BatchEntry> entries_;
    int selected_ = -1;
    QElapsedTimer clock_;
    QTimer timer_;
    qint64 started_ = 0;
    int running_ = -1;
    bool paused_ = false;
    qint64 pauseStarted_ = 0;
};
} // namespace ve
