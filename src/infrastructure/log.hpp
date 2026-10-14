#pragma once
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>

namespace ve {
class Log : public QObject {
    Q_OBJECT
  public:
    static Log& instance();
    void append(const QString& level, const QString& tag, const QString& message);
    [[nodiscard]] QString sessionPath() const;
  signals:
    void message(const QString& level, const QString& tag, const QString& text);

  private:
    Log();
    mutable QMutex mutex_;
    QFile file_;
};
void info(const QString& tag, const QString& message);
} // namespace ve
