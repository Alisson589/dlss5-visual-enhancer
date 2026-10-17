#pragma once
#include <QSize>
#include <QStringView>
#include <cstdint>
#include <memory>

namespace ve {
// Oriented opaque CUDA RGBA16F, encoded in the named media transfer.
// The owner retains the allocation and its producing native feature/device.
struct GpuRgbFrame {
    std::shared_ptr<void> owner;
    QSize size;
    uint64_t pointer=0;
    uint32_t stride=0;
    uint32_t transfer=1; // sRGB/709/linear/gamma22/gamma28/PQ/HLG = 0..6.
    uint32_t primaries=1; // Native FI convention: 0=601, 1=709, 2=2020.
    bool historyReset=false;
};
inline uint32_t nativeMediaTransfer(QStringView value){
    if(value==u"smpte2084")return 5;
    if(value==u"arib-std-b67")return 6;
    if(value==u"iec61966-2-1"||value==u"srgb")return 0;
    if(value==u"linear")return 2;
    if(value==u"gamma22"||value==u"bt470m")return 3;
    if(value==u"gamma28"||value==u"bt470bg")return 4;
    return 1;
}
} // namespace ve
