#include "image_alpha.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace ve {
namespace {
struct Kernel { int first; std::vector<double> weights; };
std::vector<Kernel> kernels(int input,int output) {
    const double ratio=double(input)/output, scale=std::max(1.,ratio), support=3*scale;
    std::vector<Kernel> result;
    result.reserve(size_t(output));
    auto sinc=[](double x) {
        if (x==0) return 1.;
        const double angle=std::numbers::pi*x;
        return std::sin(angle)/angle;
    };
    for (int position=0;position<output;++position) {
        const double center=(position+.5)*ratio;
        const int first=std::max(0,int(center-support+.5));
        const int last=std::min(input,int(center+support+.5));
        Kernel kernel{first,{}};
        double total=0;
        for (int index=first;index<last;++index) {
            const double distance=(index-center+.5)/scale;
            const double weight=std::abs(distance)<3 ? sinc(distance)*sinc(distance/3) : 0;
            kernel.weights.push_back(weight); total+=weight;
        }
        for (auto& weight : kernel.weights) weight/=total;
        result.push_back(std::move(kernel));
    }
    return result;
}
int sample(const Kernel& kernel,bool wide,const auto& value) {
    if (wide) {
        double sum=0;
        for (size_t index=0;index<kernel.weights.size();++index)
            sum+=value(kernel.first+int(index))*kernel.weights[index];
        // The reference's 16-bit mask is resampled in signed integer mode;
        // retain overshoot until both separable passes have completed.
        return int(std::round(sum));
    }
    constexpr qint64 precision=1<<22;
    qint64 sum=precision/2;
    for (size_t index=0;index<kernel.weights.size();++index)
        sum+=value(kernel.first+int(index))*qint64(std::round(kernel.weights[index]*precision));
    return int(std::clamp<qint64>(sum>>22,0,255));
}
}
void restoreImageAlpha(QImage& output,const QImage& input,JobControl& control) {
    if (!input.hasAlphaChannel()) return;
    const bool wide=input.depth()>32;
    const auto source=input.convertToFormat(QImage::Format_RGBA64);
    const int width=output.width(),height=output.height();
    std::vector<int> horizontal(size_t(width)*source.height());
    const auto xKernels=kernels(source.width(),width), yKernels=kernels(source.height(),height);
    for (int y=0;y<source.height();++y) {
        control.checkpoint();
        const auto* row=reinterpret_cast<const QRgba64*>(source.constScanLine(y));
        for (int x=0;x<width;++x)
            horizontal[size_t(y)*width+x]=sample(xKernels[size_t(x)],wide,[&](int column) {
                return int(row[column].alpha())/(wide ? 1 : 257);
            });
    }
    const auto format=wide ? QImage::Format_RGBA64 : output.format();
    output=output.convertToFormat(QImage::Format_RGBA64);
    for (int y=0;y<height;++y) {
        control.checkpoint();
        auto* row=reinterpret_cast<QRgba64*>(output.scanLine(y));
        for (int x=0;x<width;++x) {
            const int alpha=sample(yKernels[size_t(y)],wide,[&](int line) {
                return horizontal[size_t(line)*width+x];
            });
            row[x].setAlpha(quint16(std::clamp(alpha,0,wide ? 65535 : 255)*(wide ? 1 : 257)));
        }
    }
    output=output.convertToFormat(format);
}
}
