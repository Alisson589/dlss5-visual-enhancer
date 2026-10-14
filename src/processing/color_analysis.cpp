#include "color_analysis.hpp"
#include <QRgba64>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace ve {
namespace {
constexpr std::array<const char*, 12> keys{
    "lut_exposure", "lut_contrast",    "lut_highlights", "lut_shadows", "lut_whites",   "lut_blacks",
    "lut_midtones", "lut_temperature", "lut_tint",       "lut_hue",     "lut_vibrance", "lut_saturation"};
constexpr std::array<double, 12> scales{1, 100, 100, 100, 100, 100, 100, 100, 100, 90, 100, 100};
constexpr std::array<double, 12> lows{-3, -1, -1, -1, -1, -1, -1, -1, -1, -2, -1, -1};
constexpr std::array<double, 12> highs{3, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1};
using Vector = std::array<double, 12>;
float linear(float c) {
    return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
}
float encoded(float c) {
    return c <= .0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - .055f;
}
double quantileSorted(const std::vector<double>& values, double q) {
    const auto position = (values.size() - 1) * q;
    const auto low = size_t(position), high = std::min(low + 1, values.size() - 1);
    return values[low] + (values[high] - values[low]) * (position - low);
}
double quantile(std::vector<double> values, double q) {
    if (values.empty())
        return 0;
    std::ranges::sort(values);
    return quantileSorted(values, q);
}
double luminance(Rgb rgb) {
    return rgb[0] * .2126 + rgb[1] * .7152 + rgb[2] * .0722;
}
ColorSamples bounded(const ColorSamples& values, size_t count) {
    if (values.size() <= count)
        return values;
    ColorSamples result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i)
        result.push_back(values[i * (values.size() - 1) / (count - 1)]);
    return result;
}
ColorSamples lutSamples(const ColorSamples& identity, const Settings& settings) {
    if (settings.lut_path.isEmpty())
        return identity;
    const auto lut = CubeLut::load(settings.lut_path);
    ColorSamples result;
    result.reserve(identity.size());
    for (const auto rgb : identity)
        result.push_back(lut.sample(rgb));
    return result;
}
ColorGrade controls(const Vector& vector, const Settings& settings) {
    ColorGrade result(settings);
    double* fields[]{&result.lut_exposure, &result.lut_contrast,    &result.lut_highlights,
                     &result.lut_shadows,  &result.lut_whites,      &result.lut_blacks,
                     &result.lut_midtones, &result.lut_temperature, &result.lut_tint,
                     &result.lut_hue,      &result.lut_vibrance,    &result.lut_saturation};
    for (size_t i = 0; i < 12; ++i)
        *fields[i] = std::clamp(vector[i], lows[i], highs[i]) * scales[i];
    return result;
}
Vector settingsVector(const Settings& settings) {
    Vector result{};
    for (size_t i = 0; i < 12; ++i)
        result[i] = settings.value(QString::fromLatin1(keys[i])).toDouble() / scales[i];
    return result;
}
Vector mapVector(const QVariantMap& map) {
    Vector result{};
    for (size_t i = 0; i < 12; ++i)
        result[i] = map.value(QString::fromLatin1(keys[i])).toDouble() / scales[i];
    return result;
}
QVariantMap controlMap(Vector vector) {
    QVariantMap result;
    for (size_t i = 0; i < 12; ++i) {
        const double value = std::clamp(vector[i], lows[i], highs[i]) * scales[i];
        result.insert(QString::fromLatin1(keys[i]), i == 0 ? QVariant(std::nearbyint(value * 100) / 100)
                                                           : QVariant(int(std::nearbyint(value))));
    }
    return result;
}
ColorSamples grade(const ColorSamples& identity, const ColorSamples& sampled, const ColorGrade& settings) {
    ColorSamples result;
    result.reserve(identity.size());
    for (size_t i = 0; i < identity.size(); ++i)
        result.push_back(gradeRgb(identity[i], sampled[i], settings));
    return result;
}
Rgb oklab(Rgb rgb) {
    const float r = linear(rgb[0]), g = linear(rgb[1]), b = linear(rgb[2]);
    const auto l = std::cbrt(.4122214708f * r + .5363325363f * g + .0514459929f * b);
    const auto m = std::cbrt(.2119034982f * r + .6806995451f * g + .1073969566f * b);
    const auto s = std::cbrt(.0883024619f * r + .2817188376f * g + .6299787005f * b);
    return {.2104542553f * l + .793617785f * m - .0040720468f * s,
            1.6f * (1.9779984951f * l - 2.428592205f * m + .4505937099f * s),
            1.6f * (.0259040371f * l + .7827717662f * m - .808675766f * s)};
}
std::vector<double> features(const ColorSamples& samples) {
    constexpr std::array<double, 25> quantiles{.005, .01, .025, .05, .10,  .15, .20, .25, .30,
                                               .35,  .40, .45,  .50, .55,  .60, .65, .70, .75,
                                               .80,  .85, .90,  .95, .975, .99, .995};
    constexpr std::array<Rgb, 13> directions{{{1, 0, 0},
                                              {0, 1, 0},
                                              {0, 0, 1},
                                              {0, 1, 1},
                                              {0, 1, -1},
                                              {1, 1, 0},
                                              {1, -1, 0},
                                              {1, 0, 1},
                                              {1, 0, -1},
                                              {1, 1, 1},
                                              {1, 1, -1},
                                              {1, -1, 1},
                                              {1, -1, -1}}};
    std::array<std::vector<double>, 14> values;
    for (auto& list : values)
        list.reserve(samples.size());
    for (const auto rgb : samples) {
        const auto lab = oklab(rgb);
        for (size_t d = 0; d < 13; ++d) {
            const auto direction = directions[d];
            const auto length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                          direction[2] * direction[2]);
            values[d].push_back((lab[0] * direction[0] + lab[1] * direction[1] + lab[2] * direction[2]) /
                                length);
        }
        values[13].push_back(std::hypot(lab[1], lab[2]));
    }
    for (auto& list : values)
        std::ranges::sort(list);
    std::vector<double> result;
    result.reserve(350);
    for (const auto q : quantiles)
        for (size_t d = 0; d < 13; ++d)
            result.push_back(quantileSorted(values[d], q));
    for (const auto q : quantiles)
        result.push_back(quantileSorted(values[13], q));
    return result;
}
double norm(const std::vector<double>& values) {
    return std::inner_product(values.begin(), values.end(), values.begin(), 0.);
}
Vector solve(std::array<Vector, 12> matrix, Vector rhs) {
    for (size_t column = 0; column < 12; ++column) {
        size_t pivot = column;
        for (size_t row = column + 1; row < 12; ++row)
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column]))
                pivot = row;
        std::swap(matrix[column], matrix[pivot]);
        std::swap(rhs[column], rhs[pivot]);
        const double divisor = matrix[column][column];
        if (std::abs(divisor) < 1e-18)
            return {};
        for (size_t j = column; j < 12; ++j)
            matrix[column][j] /= divisor;
        rhs[column] /= divisor;
        for (size_t row = 0; row < 12; ++row)
            if (row != column) {
                const auto factor = matrix[row][column];
                for (size_t j = column; j < 12; ++j)
                    matrix[row][j] -= factor * matrix[column][j];
                rhs[row] -= factor * rhs[column];
            }
    }
    return rhs;
}
std::pair<Vector, double> fit(const ColorSamples& identity, const ColorSamples& sampled,
                              const ColorSamples& reference, const Settings& settings, Vector vector,
                              int iterations, JobControl& control) {
    const auto target = features(reference);
    const double weight = 1 / std::sqrt(double(target.size()));
    auto residual = [&](const Vector& values) {
        control.checkpoint();
        auto error = features(grade(identity, sampled, controls(values, settings)));
        for (size_t i = 0; i < target.size(); ++i)
            error[i] = (error[i] - target[i]) * weight;
        for (auto value : values)
            error.push_back(value * .00035);
        return error;
    };
    for (size_t i = 0; i < 12; ++i)
        vector[i] = std::clamp(vector[i], lows[i], highs[i]);
    auto error = residual(vector);
    double score = norm(error), damping = .0001;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        control.checkpoint();
        std::array<std::vector<double>, 12> jacobian;
        for (size_t column = 0; column < 12; ++column) {
            const double step = vector[column] + .002 <= highs[column] ? .002 : -.002;
            auto trial = vector;
            trial[column] += step;
            auto response = residual(trial);
            for (size_t j = 0; j < error.size(); ++j)
                response[j] = (response[j] - error[j]) / step;
            jacobian[column] = std::move(response);
        }
        std::array<Vector, 12> normal{};
        Vector gradient{};
        for (size_t r = 0; r < 12; ++r) {
            gradient[r] = -std::inner_product(jacobian[r].begin(), jacobian[r].end(), error.begin(), 0.);
            for (size_t c = 0; c < 12; ++c)
                normal[r][c] =
                    std::inner_product(jacobian[r].begin(), jacobian[r].end(), jacobian[c].begin(), 0.);
        }
        bool accepted = false;
        double improvement = 0;
        for (int attempt = 0; attempt < 7; ++attempt) {
            auto system = normal;
            for (size_t j = 0; j < 12; ++j)
                system[j][j] += damping;
            const auto delta = solve(system, gradient);
            auto candidate = vector;
            for (size_t j = 0; j < 12; ++j)
                candidate[j] = std::clamp(vector[j] + std::clamp(delta[j], -.35, .35), lows[j], highs[j]);
            auto candidateError = residual(candidate);
            const double candidateScore = norm(candidateError);
            if (candidateScore < score) {
                improvement = score - candidateScore;
                vector = candidate;
                error = std::move(candidateError);
                score = candidateScore;
                damping = std::max(1e-8, damping * .4);
                accepted = true;
                break;
            }
            damping *= 4;
        }
        if (!accepted || (iteration > 4 && improvement < 1e-10))
            break;
    }
    return {vector, score};
}
QRect trimmedRectangle(const QImage& image) {
    const auto black = [&](int x, int y) {
        const auto p = image.pixelColor(x, y);
        return std::max({p.redF(), p.greenF(), p.blueF()}) <= 1 / 255.;
    };
    int top = 0, bottom = 0, left = 0, right = 0;
    auto rowBlack = [&](int y) {
        int count = 0;
        for (int x = 0; x < image.width(); ++x)
            count += black(x, y);
        return count >= image.width() * .995;
    };
    auto columnBlack = [&](int x) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            count += black(x, y);
        return count >= image.height() * .995;
    };
    while (top < image.height() && rowBlack(top))
        ++top;
    while (bottom < image.height() && rowBlack(image.height() - 1 - bottom))
        ++bottom;
    while (left < image.width() && columnBlack(left))
        ++left;
    while (right < image.width() && columnBlack(image.width() - 1 - right))
        ++right;
    if (!top || !bottom || std::max(top, bottom) > std::min(top, bottom) * 2 ||
        std::max(top, bottom) > image.height() * .25)
        top = bottom = 0;
    if (!left || !right || std::max(left, right) > std::min(left, right) * 2 ||
        std::max(left, right) > image.width() * .25)
        left = right = 0;
    return {left, top, image.width() - left - right, image.height() - top - bottom};
}
} // namespace
Rgb rgbToLab(Rgb rgb) {
    const float r = linear(rgb[0]), g = linear(rgb[1]), b = linear(rgb[2]);
    auto f = [](float v) { return v > 216 / 24389.f ? std::cbrt(v) : v * (24389 / 27.f) / 116 + 16 / 116.f; };
    const auto x = f((.4124564f * r + .3575761f * g + .1804375f * b) / .95047f),
               y = f(.2126729f * r + .7151522f * g + .0721750f * b),
               z = f((.0193339f * r + .1191920f * g + .9503041f * b) / 1.08883f);
    return {116 * y - 16, 500 * (x - y), 200 * (y - z)};
}
Rgb labToRgb(Rgb lab) {
    const auto y = (lab[0] + 16) / 116, x = y + lab[1] / 500, z = y - lab[2] / 200;
    auto inverse = [](float v) { return v > 6 / 29.f ? v * v * v : (v - 16 / 116.f) * 108 / 841.f; };
    const auto X = .95047f * inverse(x), Y = inverse(y), Z = 1.08883f * inverse(z);
    Rgb result{encoded(3.2404542f * X - 1.5371385f * Y - .4985314f * Z),
               encoded(-.969266f * X + 1.8760108f * Y + .041556f * Z),
               encoded(.0556434f * X - .2040259f * Y + 1.0572252f * Z)};
    for (auto& v : result)
        v = std::clamp(v, 0.f, 1.f);
    return result;
}
ColorSamples imageColorSamples(const QImage& input, size_t limit, bool trimBorders) {
    auto image = input.convertToFormat(QImage::Format_RGBA64);
    if (trimBorders && std::max(image.width(), image.height()) > 512)
        image = image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (trimBorders)
        image = image.copy(trimmedRectangle(image));
    const int stride = std::max(1, int(std::ceil(std::max(image.width(), image.height()) / 512.)));
    ColorSamples sampled;
    sampled.reserve(size_t((image.width() + stride - 1) / stride) * ((image.height() + stride - 1) / stride));
    for (int y = 0; y < image.height(); y += stride) {
        const auto* row = reinterpret_cast<const QRgba64*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += stride)
            if (row[x].alpha() > 65535 * .05)
                sampled.push_back(
                    {row[x].red() / 65535.f, row[x].green() / 65535.f, row[x].blue() / 65535.f});
    }
    return bounded(sampled, limit);
}
QVariantMap recommendColor(const ColorSamples& identity, const Settings& settings, JobControl& control,
                           bool hdr, size_t frameCount) {
    if (identity.size() < 32)
        throw std::runtime_error("The input has too few visible pixels for Auto color adjustment.");
    control.checkpoint();
    const auto sampled = lutSamples(identity, settings);
    std::vector<double> luma;
    luma.reserve(sampled.size());
    for (auto p : sampled)
        luma.push_back(luminance(p));
    std::ranges::sort(luma);
    const auto p02 = quantileSorted(luma, .02), p50 = quantileSorted(luma, .5),
               p95 = quantileSorted(luma, .95), p98 = quantileSorted(luma, .98),
               p99 = quantileSorted(luma, .99);
    auto values = controlMap({});
    values["lut_mix"] = 100;
    if (p98 - p02 < .06)
        return values;
    double exposure =
        std::clamp(.55 * std::log2(linear(.82f) / std::max(double(linear(float(p95))), .003)), -.7, 1.2);
    if (p99 > .75)
        exposure =
            std::min(exposure, .8 * std::log2(linear(.98f) / std::max(double(linear(float(p99))), .003)));
    if (p50 < .22 && p99 > .7)
        exposure = std::min(exposure, .25);
    if (p50 > .72 && p02 > .2)
        exposure = std::max(exposure, -.25);
    if (hdr)
        exposure = std::clamp(exposure, -.25, .25);
    exposure = std::nearbyint(exposure / .05) * .05;
    ColorGrade base;
    base.lut_exposure = exposure;
    const auto exposed = grade(sampled, sampled, base);
    std::vector<double> levels;
    levels.reserve(exposed.size());
    for (auto p : exposed)
        levels.push_back(luminance(p));
    std::ranges::sort(levels);
    const auto e02 = quantileSorted(levels, .02), e10 = quantileSorted(levels, .1),
               e50 = quantileSorted(levels, .5), e90 = quantileSorted(levels, .9),
               e98 = quantileSorted(levels, .98), e99 = quantileSorted(levels, .99);
    auto rounded = [](double v, double low, double high) {
        return int(std::nearbyint(std::clamp(v, low, high)));
    };
    values["lut_exposure"] = exposure;
    values["lut_contrast"] = rounded((.64 / std::max(e90 - e10, .15) - 1) * 20, -12, 20);
    values["lut_shadows"] = rounded((.13 - e10) * 115, 0, 18);
    values["lut_highlights"] = -rounded((e99 - .92) * 140, 0, 25);
    values["lut_whites"] = rounded((.86 - e98) * 60, 0, 12);
    values["lut_blacks"] = -rounded((e02 - .05) * 130, 0, 18);
    values["lut_midtones"] = e99 > .7 ? rounded((.30 - e50) * 45, 0, 10) : 0;
    std::array<std::vector<double>, 3> neutral;
    double chromaSum = 0;
    size_t chromaCount = 0;
    for (const auto rgb : sampled) {
        const auto level = luminance(rgb);
        const auto chroma =
            *std::max_element(rgb.begin(), rgb.end()) - *std::min_element(rgb.begin(), rgb.end());
        if (level > .12 && level < .9) {
            chromaSum += chroma;
            ++chromaCount;
        }
        if (chroma < .055 && level > .18 && level < .88)
            for (size_t c = 0; c < 3; ++c)
                neutral[c].push_back(rgb[c]);
    }
    if (settings.lut_path.isEmpty() && !hdr &&
        neutral[0].size() >= std::max(size_t(100), sampled.size() / 50)) {
        const double r = quantile(neutral[0], .5), g = quantile(neutral[1], .5), b = quantile(neutral[2], .5),
                     level = std::max((r + g + b) / 3, .1);
        values["lut_temperature"] = rounded((b - r) / level * 125, -16, 16);
        values["lut_tint"] = rounded((g - (r + b) * .5) / level * 220, -12, 12);
    }
    const double mean = chromaCount ? chromaSum / chromaCount : 0;
    values["lut_vibrance"] = mean > .035 ? rounded((.2 - mean) * 75, 0, 16) : 0;
    values["lut_saturation"] = -rounded((mean - .4) * 30, 0, 8);
    frameCount = std::max(size_t(1), frameCount);
    if (sampled.size() % frameCount)
        frameCount = 1;
    auto clipping = [&](const ColorSamples& pixels) {
        std::vector<std::array<double, 2>> result(frameCount);
        const size_t count = pixels.size() / frameCount;
        for (size_t i = 0; i < pixels.size(); ++i) {
            const auto rgb = pixels[i];
            result[i / count][0] += *std::max_element(rgb.begin(), rgb.end()) >= .997;
            result[i / count][1] += *std::max_element(rgb.begin(), rgb.end()) <= .003;
        }
        for (auto& frame : result)
            for (auto& rate : frame)
                rate /= count;
        return result;
    };
    const auto baseline = clipping(sampled);
    for (int attempt = 0; attempt < (frameCount > 1 ? 32 : 8); ++attempt) {
        control.checkpoint();
        const auto rates = clipping(grade(identity, sampled, controls(mapVector(values), Settings{})));
        bool white = false, black = false;
        for (size_t i = 0; i < frameCount; ++i) {
            white |= rates[i][0] > baseline[i][0] + .012;
            black |= rates[i][1] > baseline[i][1] + .012;
        }
        if (!white && !black)
            break;
        if (white) {
            const int whites = values.value("lut_whites").toInt();
            const double e = values.value("lut_exposure").toDouble();
            if (whites > 0)
                values["lut_whites"] = std::max(0, whites - 3);
            else if (e > 0)
                values["lut_exposure"] = std::max(0., e - .05);
            else
                values["lut_highlights"] = std::max(-35, values.value("lut_highlights").toInt() - 4);
        }
        if (black) {
            values["lut_blacks"] = std::min(0, values.value("lut_blacks").toInt() + 3);
            values["lut_contrast"] = std::max(-12, values.value("lut_contrast").toInt() - 2);
        }
    }
    return values;
}
QVariantMap fitReferenceColor(const ColorSamples& source, const ColorSamples& reference,
                              const Settings& settings, JobControl& control) {
    if (settings.lut_mix <= 0)
        throw std::runtime_error("Increase LUT Strength above 0 before matching a reference image.");
    if (source.size() < 32 || reference.size() < 32)
        throw std::runtime_error("The input or reference has too few visible colors to analyze.");
    const auto sampled = lutSamples(source, settings), coarse = bounded(source, 2048),
               coarseSampled = bounded(sampled, 2048), coarseReference = bounded(reference, 2048);
    std::array<Vector, 3> seeds{};
    seeds[1] = settingsVector(settings);
    std::vector<double> sourceLevel, targetLevel;
    for (auto p : coarseSampled)
        sourceLevel.push_back(oklab(p)[0]);
    for (auto p : coarseReference)
        targetLevel.push_back(oklab(p)[0]);
    seeds[2][0] = std::clamp(
        3 * std::log2(std::max(.03, quantile(targetLevel, .5)) / std::max(.03, quantile(sourceLevel, .5))),
        -3., 3.);
    Vector best{};
    double bestScore = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < seeds.size(); ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j)
            duplicate |= seeds[i] == seeds[j];
        if (duplicate)
            continue;
        const auto [vector, score] =
            fit(coarse, coarseSampled, coarseReference, settings, seeds[i], 35, control);
        if (score < bestScore) {
            best = vector;
            bestScore = score;
        }
    }
    const auto fine = bounded(source, 12288), fineSampled = bounded(sampled, 12288),
               fineReference = bounded(reference, 12288);
    best = fit(fine, fineSampled, fineReference, settings, best, 45, control).first;
    auto values = controlMap(best);
    const auto target = features(fineReference);
    auto score = [&](const QVariantMap& candidate) {
        control.checkpoint();
        auto response = features(grade(fine, fineSampled, controls(mapVector(candidate), settings)));
        for (size_t i = 0; i < target.size(); ++i)
            response[i] -= target[i];
        return norm(response);
    };
    bestScore = score(values);
    for (int pass = 0; pass < 2; ++pass)
        for (size_t key = 0; key < 12; ++key)
            for (int direction : {-1, 1}) {
                const auto name = QString::fromLatin1(keys[key]);
                const double next = values.value(name).toDouble() + direction * (key == 0 ? .01 : 1);
                if (next < lows[key] * scales[key] || next > highs[key] * scales[key])
                    continue;
                auto candidate = values;
                candidate[name] = key == 0 ? QVariant(std::nearbyint(next * 100) / 100) : QVariant(int(next));
                const auto candidateScore = score(candidate);
                if (candidateScore < bestScore) {
                    values = std::move(candidate);
                    bestScore = candidateScore;
                }
            }
    return values;
}
std::vector<double> fixedColorFrameTimes(double duration, double fps) {
    if (!std::isfinite(duration) || duration <= 0)
        throw std::runtime_error("Video duration is unavailable; reload the input and try Auto again.");
    std::vector<double> times;
    for (int i = 0; i < 15; ++i) {
        double seconds = duration * (.05 + .9 * i / 14);
        if (fps > 0)
            seconds =
                std::min(std::max(0., std::nearbyint(duration * fps) - 1), std::floor(seconds * fps + 1e-6)) /
                fps;
        if (times.empty() || seconds != times.back())
            times.push_back(seconds);
    }
    return times;
}
ColorSamples combinedVideoSamples(std::vector<ColorSamples> frames, size_t* usedFrames) {
    std::vector<ColorSamples> usable;
    for (const auto& frame : frames) {
        if (frame.size() < 32)
            continue;
        std::vector<double> levels;
        for (auto p : frame)
            levels.push_back(luminance(p));
        std::ranges::sort(levels);
        if (quantileSorted(levels, .98) > .025 && quantileSorted(levels, .02) < .975)
            usable.push_back(frame);
    }
    if (usable.empty())
        usable = std::move(frames);
    if (usable.empty())
        throw std::runtime_error("Could not find a usable video frame for Auto color adjustment.");
    size_t count = 220000 / usable.size();
    for (const auto& frame : usable)
        count = std::min(count, frame.size());
    if (count < 32)
        throw std::runtime_error("The input has too few valid colors for Auto adjustment.");
    if (usedFrames)
        *usedFrames = usable.size();
    ColorSamples result;
    result.reserve(count * usable.size());
    for (const auto& frame : usable) {
        const auto sample = bounded(frame, count);
        result.insert(result.end(), sample.begin(), sample.end());
    }
    return result;
}
} // namespace ve
