#pragma once
#include "av_api.hpp"
#include "infrastructure/process.hpp"
#include <cstring>
#include <exception>

namespace ve::av {
// Non-seekable AVIO owns its buffer and returns C++ failures across the C ABI.
// The format context must close before this object is destroyed.
class PipeIo {
  public:
    PipeProcess& process;
    JobControl& control;
    std::exception_ptr failure;
    PipeIo(PipeProcess& source,JobControl& job)
        : process(source),control(job) {
        auto& api=Api::get();
        auto* buffer=static_cast<uint8_t*>(api.av_malloc(65536));
        if (!buffer) throw std::bad_alloc{};
        io_=api.avio_alloc_context(buffer,65536,0,this,read,nullptr,nullptr);
        if (!io_) { api.av_free(buffer); throw std::bad_alloc{}; }
    }
    ~PipeIo() {
        if (io_) { auto& api=Api::get(); api.av_free(io_->buffer); api.avio_context_free(&io_); }
    }
    PipeIo(const PipeIo&)=delete;
    PipeIo& operator=(const PipeIo&)=delete;
    [[nodiscard]] AVIOContext* context() const { return io_; }
    void check(int result,const char* operation="Timestamped media transport") const {
        if (failure) std::rethrow_exception(failure);
        av::check(result,operation);
    }
  private:
    AVIOContext* io_=nullptr;
    static int read(void* opaque,uint8_t* buffer,int size) noexcept {
        auto& pipe=*static_cast<PipeIo*>(opaque);
        try {
            const auto bytes=pipe.process.readSome(size,pipe.control);
            if (bytes.isEmpty()) return AVERROR_EOF;
            std::memcpy(buffer,bytes.constData(),size_t(bytes.size()));
            return int(bytes.size());
        } catch (...) { pipe.failure=std::current_exception(); return AVERROR_EXIT; }
    }
};
}
