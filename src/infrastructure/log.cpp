#include "log.hpp"
#include "paths.hpp"
#include <QDateTime>
#include <QMutexLocker>
#include <QThread>

namespace ve {
Log::Log() {
    file_.setFileName(Paths::instance().at(
        "logs/session-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".log"));
    (void)file_.open(QIODevice::WriteOnly | QIODevice::Append);
}
Log& Log::instance() {
    static Log log;
    return log;
}
void Log::append(const QString& level, const QString& tag, const QString& messageText) {
    {
        QMutexLocker lock(&mutex_);
        file_.write((QDateTime::currentDateTime().toString("HH:mm:ss.zzz") + " [" + level + "][" + tag +
                     "] " + messageText + '\n')
                        .toUtf8());
        file_.flush();
    }
    emit message(level, tag, messageText);
}
QString Log::sessionPath() const {
    QMutexLocker lock(&mutex_);
    return file_.fileName();
}
void info(const QString& tag, const QString& message) {
    Log::instance().append("INFO", tag, message);
}
} // namespace ve
