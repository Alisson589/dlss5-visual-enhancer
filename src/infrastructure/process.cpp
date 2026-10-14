#include "process.hpp"
#include <QElapsedTimer>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ve {
namespace {
void start(QProcess& process, const QString& program, const QStringList& arguments) {
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= 0x08000000; // CREATE_NO_WINDOW: child tools never flash a console.
        arguments->startupInfo->dwFlags |= STARTF_FORCEOFFFEEDBACK;
    });
#endif
    process.setProgram(program);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(10000))
        throw std::runtime_error(process.errorString().toStdString());
}
} // namespace
ProcessResult runProcess(const QString& program, const QStringList& arguments, JobControl* control,
                         int timeoutMs, const QByteArray& input, const std::function<void(const QByteArray&)>& outputReady) {
    QProcess process;
    start(process, program, arguments);
    QElapsedTimer clock;
    clock.start();
    ProcessResult result;
    qsizetype written = 0;
    bool closed = false;
    try {
        while (process.state() != QProcess::NotRunning) {
            if (control)
                control->checkpoint();
            if (timeoutMs > 0 && clock.elapsed() > timeoutMs)
                throw std::runtime_error("External process timed out.");
            if (written < input.size() && process.bytesToWrite() < 1024 * 1024) {
                const auto amount = std::min<qsizetype>(1024 * 1024, input.size() - written);
                const auto accepted = process.write(input.constData() + written, amount);
                if (accepted < 0)
                    throw std::runtime_error("External process closed its input.");
                written += accepted;
            } else if (!closed && written == input.size()) {
                process.closeWriteChannel();
                closed = true;
            }
            process.waitForReadyRead(20);
            const auto output=process.readAllStandardOutput();if(outputReady&&!output.isEmpty())outputReady(output);result.output += output;
            if(outputReady&&result.output.size()>1024*1024)result.output=result.output.right(1024*1024);
            result.error += process.readAllStandardError();
            if (result.error.size() > 2 * 1024 * 1024)
                result.error = result.error.right(1024 * 1024);
        }
    } catch (...) {
        process.kill();
        process.waitForFinished(3000);
        throw;
    }
    const auto output=process.readAllStandardOutput();if(outputReady&&!output.isEmpty())outputReady(output);result.output += output;
    result.error += process.readAllStandardError();
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return result;
}
PipeProcess::PipeProcess(const QString& program, const QStringList& arguments) {
    start(process_, program, arguments);
}
PipeProcess::~PipeProcess() {
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(3000);
    }
}
QByteArray PipeProcess::readSome(qsizetype maximum, JobControl& control) {
    while (true) {
        control.checkpoint();
        drainErrors();
        auto data = process_.read(maximum);
        if (!data.isEmpty())
            return data;
        if (process_.state() == QProcess::NotRunning) {
            if (process_.exitCode() != 0)
                throw std::runtime_error(error_.toStdString());
            return {};
        }
        process_.waitForReadyRead(25);
    }
}
QByteArray PipeProcess::readAvailable(qsizetype maximum, JobControl& control) {
    control.checkpoint();
    process_.waitForReadyRead(0);
    drainErrors();
    auto result=process_.read(maximum);
    if (result.isEmpty() && process_.state()==QProcess::NotRunning && process_.exitCode()!=0)
        throw std::runtime_error(error_.toStdString());
    return result;
}
void PipeProcess::drainErrors() {
    error_ += process_.readAllStandardError();
    if (error_.size() > 1024 * 1024)
        error_ = error_.right(512 * 1024);
}
QByteArray PipeProcess::read(qsizetype size, JobControl& control) {
    QByteArray data;
    data.reserve(size);
    while (data.size() < size) {
        control.checkpoint();
        data += process_.read(size - data.size());
        drainErrors();
        if (data.size() == size)
            break;
        if (process_.state() == QProcess::NotRunning) {
            data += process_.read(size - data.size());
            break;
        }
        process_.waitForReadyRead(25);
    }
    if (!data.isEmpty() && data.size() != size)
        throw std::runtime_error("Decoder returned an incomplete video frame.");
    if (data.isEmpty() && process_.state() == QProcess::NotRunning && process_.exitCode() != 0)
        throw std::runtime_error(error_.toStdString());
    return data;
}
void PipeProcess::write(const char* data, qsizetype size, JobControl& control) {
    qsizetype offset = 0;
    while (offset < size) {
        control.checkpoint();
        drainErrors();
        if (process_.state() == QProcess::NotRunning)
            throw std::runtime_error(error_.toStdString());
        if (process_.bytesToWrite() > 4 * 1024 * 1024) {
            // A filter writes while it reads. Pump both pipe directions so a
            // full stdout pipe cannot prevent it from consuming our input.
            process_.waitForBytesWritten(20);
            continue;
        }
        const auto accepted = process_.write(data + offset, std::min<qsizetype>(1024 * 1024, size - offset));
        if (accepted <= 0)
            throw std::runtime_error("Encoder closed its input: " + error_.toStdString());
        offset += accepted;
    }
    while (process_.bytesToWrite() > 0) {
        control.checkpoint();
        drainErrors();
        if (process_.state() == QProcess::NotRunning)
            throw std::runtime_error(error_.toStdString());
        // Qt pumps stdout/stderr during this wait as well. Wake on the write
        // itself: delivery muxers have no stdout and would otherwise incur a
        // full polling timeout for every compressed video/audio packet.
        process_.waitForBytesWritten(25);
    }
}
void PipeProcess::closeInput() {
    process_.closeWriteChannel();
}
void PipeProcess::finish(JobControl& control) {
    while (process_.state() != QProcess::NotRunning) {
        control.checkpoint();
        drainErrors();
        process_.waitForFinished(25);
    }
    drainErrors();
    if (process_.exitCode() != 0 || process_.exitStatus() != QProcess::NormalExit)
        throw std::runtime_error(error_.toStdString());
}
QString PipeProcess::error() {
    drainErrors();
    return QString::fromUtf8(error_);
}
} // namespace ve
