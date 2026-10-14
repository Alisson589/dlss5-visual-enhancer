#pragma once
#include <QImage>
#include <QRgba64>
#include <algorithm>
#include <cmath>
#include <vector>
#include <array>

namespace ve {
// Same reduced area/luma decision as the reference's software temporal guide.
// Each stage keeps its own previous signature across batch boundaries.
class TemporalGuide {
  public:
    [[nodiscard]] bool reset(const QImage& input, double threshold, bool explicitReset = false) {
        const double scale = std::min(1., 640. / input.width());
        const QSize size(std::max(32, int(std::nearbyint(input.width() * scale / 2)) * 2),
                         std::max(32, int(std::nearbyint(input.height() * scale / 2)) * 2));
        const auto image = input.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                               .convertToFormat(QImage::Format_RGBA64);
        std::vector<uchar> current(size_t(size.width()) * size.height());
        for (int y = 0; y < size.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgba64*>(image.constScanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                // OpenCV's RGBA2GRAY weights; retain the reference's high-depth >>8 rule.
                const auto luma = (uint64_t(row[x].red()) * 19595 + uint64_t(row[x].green()) * 38470 +
                                   uint64_t(row[x].blue()) * 7471 + 32768) >> 16;
                current[size_t(y) * size.width() + x] = uchar(luma >> 8);
            }
        }
        const bool first = explicitReset || previousSize_ != size || previous_.size() != current.size();
        std::uint64_t difference = 0;
        if (!first)
            for (size_t i = 0; i < current.size(); ++i)
                difference += std::abs(int(current[i]) - int(previous_[i]));
        score_ = first ? 1 : double(difference) / (current.size() * 255.);
        previous_ = std::move(current);
        previousSize_ = size;
        return first || score_ > threshold;
    }
    [[nodiscard]] double score() const { return score_; }

    // Media SR needs cuts separated from camera motion and brief flashes.
    // Histograms are spatially invariant; chroma catches equal-luma cuts.
    [[nodiscard]] bool resetMedia(const QImage& input, bool explicitReset = false) {
        const auto image=input.scaled(96,64,Qt::IgnoreAspectRatio,Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGBA8888);
        std::array<double,48> histogram{};
        double mean=0;
        std::vector<uchar> colors(96*64*3);
        for(int y=0;y<64;++y){
            const auto* row=image.constScanLine(y);
            for(int x=0;x<96;++x){
                const int r=row[x*4],g=row[x*4+1],b=row[x*4+2];
                const int l=(54*r+183*g+19*b+128)>>8;
                const int u=std::clamp(128+(b-l)/2,0,255),v=std::clamp(128+(r-l)/2,0,255);
                ++histogram[l/16];++histogram[16+u/16];++histogram[32+v/16];
                const auto i=size_t(y*96+x)*3;colors[i]=uchar(l);colors[i+1]=uchar(u);colors[i+2]=uchar(v);mean+=l;
            }
        }
        mean/=96*64;
        const bool first=explicitReset||mediaPrevious_.empty()||mediaInputSize_!=input.size();
        double histogramLuma=0,histogramChroma=0,colorDifference=0,spatialDifference=0;
        if(!first){
            for(int i=0;i<48;++i){
                const double difference=std::abs(histogram[i]-mediaHistogram_[i])/(2.*96*64);
                if(i<16)histogramLuma+=difference;else histogramChroma+=difference*.5;
            }
            const double exposure=mean-mediaMean_;
            for(size_t i=0;i<colors.size();i+=3){
                spatialDifference+=std::min(255.,std::abs(double(colors[i])-mediaPrevious_[i]-exposure));
                colorDifference+=std::abs(int(colors[i+1])-mediaPrevious_[i+1])+std::abs(int(colors[i+2])-mediaPrevious_[i+2]);
            }
            spatialDifference/=96*64*255.;colorDifference/=2.*96*64*255.;
        }
        mediaPrevious_=std::move(colors);mediaHistogram_=histogram;mediaMean_=mean;mediaInputSize_=input.size();
        score_=first?1:std::max(histogramLuma,histogramChroma);
        return first || (histogramChroma>.30&&colorDifference>.06) ||
            (histogramLuma>.42&&spatialDifference>.09) ||
            (spatialDifference>.20&&histogramChroma>.12);
    }

  private:
    QSize previousSize_;
    std::vector<uchar> previous_;
    double score_ = 1;
    std::vector<uchar> mediaPrevious_;
    std::array<double,48> mediaHistogram_{};
    QSize mediaInputSize_;
    double mediaMean_=0;
};
} // namespace ve
