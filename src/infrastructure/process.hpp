#pragma once
#include "job.hpp"
#include <QByteArray>
#include <QProcess>
#include <QStringList>
#include <memory>
#include <functional>

namespace ve {
struct ProcessResult {
    int exitCode = -1;
    QByteArray output;
    QByteArray error;
};
[[nodiscard]] ProcessResult runProcess(const QString& program, const QStringList& arguments,
                                       JobControl* control = nullptr, int timeoutMs = 30000,
                                       const QByteArray& input = {}, const std::function<void(const QByteArray&)>& outputReady = {});
class PipeProcess {
  public:
    PipeProcess(const QString& program, const QStringList& arguments);
    ~PipeProcess();
    PipeProcess(const PipeProcess&) = delete;
    PipeProcess& operator=(const PipeProcess&) = delete;
    [[nodiscard]] QByteArray read(qsizetype size, JobControl& control);
    [[nodiscard]] QByteArray readSome(qsizetype maximum, JobControl& control);
    [[nodiscard]] QByteArray readAvailable(qsizetype maximum, JobControl& control);
    void write(const char* data, qsizetype size, JobControl& control);
    void closeInput();
    void finish(JobControl& control);
    [[nodiscard]] QString error();

  private:
    QProcess process_;
    QByteArray error_;
    void drainErrors();
};
} // namespace ve
