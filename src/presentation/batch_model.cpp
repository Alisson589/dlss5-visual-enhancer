#include "batch_model.hpp"
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace ve {
BatchModel::BatchModel(QObject* parent) : QAbstractListModel(parent) {
    clock_.start();
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (paused_ || running_ < 0 || running_ >= count())
            return;
        entries_[size_t(running_)].elapsed = (clock_.elapsed() - started_) / 1000.;
        emit dataChanged(index(running_), index(running_), {ElapsedSeconds, RemainingSeconds});
    });
    timer_.start();
}
int BatchModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}
QVariant BatchModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= count())
        return {};
    const auto& e = entries_[size_t(idx.row())];
    switch (role) {
    case Qt::DisplayRole:
    case FileName:
        return QFileInfo(e.input).fileName();
    case InputPath:
        return e.input;
    case OutputPath:
        return e.output;
    case ItemState:
        return e.state;
    case Progress:
        return e.progress;
    case Detail:
        return e.detail;
    case ElapsedSeconds:
        return e.elapsed;
    case InputDimensions:
        return e.inputDimensions;
    case OutputDimensions:
        return e.outputDimensions;
    case ThumbnailUrl:
        return e.thumbnail;
    case ItemIndex:
        return idx.row();
    case Selected:
        return idx.row() == selected_;
    case FileSizeBytes:
        return double(e.bytes);
    case DurationSeconds:
        return e.duration;
    case FrameCount:
        return e.frameCount;
    case ProcessedFrames:
        return e.processedFrames;
    case ProcessingTotalFrames:
        return e.totalFrames;
    case ProcessingFps:
        return e.processingFps;
    case RemainingSeconds:
        return e.remaining;
    default:
        return {};
    }
}
QHash<int, QByteArray> BatchModel::roleNames() const {
    return {{FileName, "fileName"},
            {InputPath, "inputPath"},
            {OutputPath, "outputPath"},
            {ItemState, "itemState"},
            {Progress, "progress"},
            {Detail, "detail"},
            {ElapsedSeconds, "elapsedSeconds"},
            {InputDimensions, "inputDimensions"},
            {OutputDimensions, "outputDimensions"},
            {ThumbnailUrl, "thumbnailUrl"},
            {ItemIndex, "itemIndex"},
            {Selected, "selected"},
            {FileSizeBytes, "fileSizeBytes"},
            {DurationSeconds, "durationSeconds"},
            {FrameCount, "frameCount"},
            {ProcessedFrames, "processedFrames"},
            {ProcessingTotalFrames, "processingTotalFrames"},
            {ProcessingFps, "processingFps"},
            {RemainingSeconds, "remainingSeconds"}};
}
QString BatchModel::selectedPath() const {
    return selected_ >= 0 && selected_ < count() ? entries_[size_t(selected_)].input : QString{};
}
QStringList BatchModel::paths() const {
    QStringList paths;
    for (const auto& e : entries_)
        paths.append(e.input);
    return paths;
}
const BatchEntry& BatchModel::entry(int index) const {
    return entries_.at(size_t(index));
}
int BatchModel::indexOf(const QString& path) const {
    for (int row = 0; row < count(); ++row)
        if (entries_[size_t(row)].input == path)
            return row;
    return -1;
}
void BatchModel::add(const QStringList& paths, std::shared_ptr<void> inputOwner) {
    QSet<QString> existing;
    for (const auto& e : entries_)
        existing.insert(e.input.toCaseFolded());
    std::vector<BatchEntry> added;
    for (const auto& path : paths) {
        if (existing.contains(path.toCaseFolded()))
            continue;
        existing.insert(path.toCaseFolded());
        BatchEntry entry;
        entry.input = path;
        entry.inputOwner=inputOwner;
        entry.bytes = QFileInfo(path).size();
        added.push_back(std::move(entry));
    }
    if (added.empty())
        return;
    const auto start = count();
    beginInsertRows({}, start, start + int(added.size()) - 1);
    for (auto& e : added)
        entries_.push_back(std::move(e));
    endInsertRows();
    emit countChanged();
    if (selected_ < 0)
        select(0);
}
void BatchModel::select(int selected) {
    if (selected < -1 || selected >= count() || selected == selected_)
        return;
    const auto previous = selected_;
    selected_ = selected;
    if (previous >= 0)
        emit dataChanged(index(previous), index(previous), {Selected});
    if (selected_ >= 0)
        emit dataChanged(index(selected_), index(selected_), {Selected});
    emit selectedIndexChanged();
}
void BatchModel::remove(int row) {
    if (row < 0 || row >= count())
        return;
    beginRemoveRows({}, row, row);
    entries_.erase(entries_.begin() + row);
    endRemoveRows();
    if (selected_ == row)
        selected_ = std::min(row, count() - 1);
    else if (selected_ > row)
        --selected_;
    emit countChanged();
    emit selectedIndexChanged();
    if (count())
        emit dataChanged(index(0), index(count() - 1));
}
void BatchModel::clear() {
    beginResetModel();
    entries_.clear();
    selected_ = -1;
    running_ = -1;
    endResetModel();
    emit countChanged();
    emit selectedIndexChanged();
}
void BatchModel::clearCompleted() {
    for (int row = count() - 1; row >= 0; --row)
        if (entries_[size_t(row)].state == "Done" || entries_[size_t(row)].state == "Completed")
            remove(row);
}
void BatchModel::retryFailed() {
    for (int row = 0; row < count(); ++row)
        if (entries_[size_t(row)].state == "Failed" || entries_[size_t(row)].state == "Cancelled")
            update(row, "Queued", 0, {});
}
void BatchModel::resetStates() {
    for (int row = 0; row < count(); ++row)
        update(row, "Queued", 0, {});
}
void BatchModel::update(int row, const QString& state, double fraction, const QString& message) {
    if (row < 0 || row >= count())
        return;
    auto& e = entries_[size_t(row)];
    if (state == "Running" && e.state != "Running") {
        running_ = row;
        started_ = clock_.elapsed();
        e.elapsed = 0;
        e.processedFrames = 0;
        e.remaining = 0;
    }
    if (state != "Running" && running_ == row) {
        e.elapsed = ((paused_ ? pauseStarted_ : clock_.elapsed()) - started_) / 1000.;
        running_ = -1;
    }
    e.state = state;
    e.progress = fraction;
    e.detail = message;
    emit dataChanged(index(row), index(row));
}
void BatchModel::metadata(int row, const MediaInfo& info, const QString& thumbnail) {
    if (row < 0 || row >= count())
        return;
    auto& e = entries_[size_t(row)];
    e.source = info;
    e.inputDimensions = QString::number(info.width) + QChar(0xD7) + QString::number(info.height);
    e.duration = info.duration;
    e.frameCount = info.frames;
    if (!thumbnail.isEmpty())
        e.thumbnail = thumbnail;
    emit dataChanged(index(row), index(row));
}
void BatchModel::completed(int row, const QString& output, const MediaInfo& info) {
    if (row < 0 || row >= count())
        return;
    auto& e = entries_[size_t(row)];
    e.output = output;
    e.result = info;
    e.remaining = 0;
    e.outputDimensions = QString::number(info.width) + QChar(0xD7) + QString::number(info.height);
    update(row, "Completed", 1, "Complete");
}
void BatchModel::metrics(int row, qint64 processed, qint64 total, double fps) {
    if (row < 0 || row >= count())
        return;
    auto& e = entries_[size_t(row)];
    e.processedFrames = processed;
    e.totalFrames = total;
    e.processingFps = fps;
    e.remaining = fps > 0 ? std::max(0., (total - processed) / fps) : 0;
    emit dataChanged(index(row), index(row));
}
void BatchModel::setPaused(bool paused) {
    if (paused == paused_)
        return;
    if (paused) {
        pauseStarted_ = clock_.elapsed();
        if (running_ >= 0)
            entries_[size_t(running_)].elapsed = (pauseStarted_ - started_) / 1000.;
    } else if (running_ >= 0)
        started_ += clock_.elapsed() - pauseStarted_;
    paused_ = paused;
    if (running_ >= 0)
        emit dataChanged(index(running_), index(running_));
}
} // namespace ve
