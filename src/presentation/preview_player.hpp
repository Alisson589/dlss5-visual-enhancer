#pragma once
#include "infrastructure/job.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ve {
class PreviewPlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QObject* videoOutput READ videoOutput WRITE setVideoOutput NOTIFY videoOutputChanged)
    Q_PROPERTY(int loops READ loops WRITE setLoops NOTIFY loopsChanged)
    Q_PROPERTY(int position READ position WRITE seek NOTIFY positionChanged)
    Q_PROPERTY(int duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double videoFps READ videoFps NOTIFY videoFpsChanged)
    Q_PROPERTY(double playbackRate READ playbackRate WRITE setPlaybackRate NOTIFY playbackRateChanged)
    Q_PROPERTY(int mediaStatus READ mediaStatus NOTIFY mediaStatusChanged)
    Q_PROPERTY(int playbackState READ playbackState NOTIFY playbackStateChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
  public:
    explicit PreviewPlayer(QObject* parent = nullptr);
    ~PreviewPlayer() override;
    QUrl source() const { return source_; }
    QObject* videoOutput() const { return output_; }
    int loops() const { return loops_; }
    int position() const { return position_; }
    int duration() const { return duration_; }
    double videoFps() const { return fps_; }
    double playbackRate() const { return rate_; }
    int mediaStatus() const { return status_; }
    int playbackState() const { return state_; }
    QString errorString() const { return error_; }
    void setSource(const QUrl& source);
    void setVideoOutput(QObject* output);
    void setLoops(int loops) {
        if (loops != loops_) {
            loops_ = loops;
            emit loopsChanged();
        }
    }
    void setPlaybackRate(double rate);
    Q_INVOKABLE void seek(int position);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void close();
  signals:
    void sourceChanged();
    void videoOutputChanged();
    void loopsChanged();
    void positionChanged(int value);
    void durationChanged(int value);
    void videoFpsChanged();
    void mediaStatusChanged(int value);
    void playbackStateChanged(int value);
    void errorOccurred(int code, QString message);
    void errorStringChanged();
    void playbackRateChanged();

  private:
    struct Request {
        QString path;
        int position = 0;
        quint64 generation = 0;
    };
    struct Frame {
        QVideoFrame video;
        int position = 0;
        quint64 generation = 0;
    };
    void restart();
    void tick();
    void decode(std::stop_token stop);
    void setState(int state);
    void setStatus(int status);
    QUrl source_;
    QPointer<QObject> output_;
    QPointer<QVideoSink> sink_;
    int loops_ = 1, completedLoops_ = 0, position_ = 0, duration_ = 0, status_ = 0, state_ = 0;
    double fps_ = 0, rate_ = 1;
    QString error_;
    QTimer timer_;
    QElapsedTimer clock_;
    int clockPosition_ = 0;
    bool primed_ = false, finished_ = false, closed_ = false;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Frame> queue_;
    std::optional<Request> request_;
    std::shared_ptr<JobControl> control_;
    quint64 generation_ = 0;
    std::jthread thread_;
};
} // namespace ve
