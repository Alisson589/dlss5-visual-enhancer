#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <functional>

namespace ve {
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Cancelled") {}
};
class JobControl {
  public:
    void cancel() {
        stop_.request_stop();
        pause(false);
        std::function<void(bool,bool)> notify;{std::lock_guard lock(mutex_);notify=stateChanged_;}if(notify)notify(false,true);
    }
    void pause(bool value) {
        {
            std::lock_guard lock(mutex_);
            if (pause_.load() == value)
                return;
            const auto now = std::chrono::steady_clock::now();
            if (value)
                pauseStarted_ = now;
            else
                pausedTime_ += now - pauseStarted_;
            pause_.store(value);
        }
        condition_.notify_all();
        std::function<void(bool,bool)> notify;{std::lock_guard lock(mutex_);notify=stateChanged_;}if(notify)notify(paused(),cancelled());
    }
    [[nodiscard]] std::chrono::milliseconds pausedTime() const {
        std::lock_guard lock(mutex_);
        const auto current = pausedTime_ + (paused() ? std::chrono::steady_clock::now() - pauseStarted_
                                                     : std::chrono::steady_clock::duration{});
        return std::chrono::duration_cast<std::chrono::milliseconds>(current);
    }
    [[nodiscard]] bool paused() const { return pause_.load(); }
    [[nodiscard]] bool cancelled() const { return stop_.stop_requested(); }
    void checkpoint() {
        if(externalCheckpoint_)externalCheckpoint_();
        if (cancelled())
            throw Cancelled{};
        if (paused()) {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stop_.get_token(), [this] { return !paused(); });
        }
        if (cancelled())
            throw Cancelled{};
    }
    void setStateChanged(std::function<void(bool,bool)> callback){std::lock_guard lock(mutex_);stateChanged_=std::move(callback);}
    void setExternalCheckpoint(std::function<void()> callback){externalCheckpoint_=std::move(callback);}
    void accountExternalPause(std::chrono::steady_clock::duration duration){std::lock_guard lock(mutex_);pausedTime_+=duration;}

  private:
    std::stop_source stop_;
    std::function<void(bool,bool)> stateChanged_;
    std::function<void()> externalCheckpoint_;
    std::atomic<bool> pause_{false};
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::chrono::steady_clock::time_point pauseStarted_;
    std::chrono::steady_clock::duration pausedTime_{};
};
} // namespace ve
