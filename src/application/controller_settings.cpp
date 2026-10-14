#include "controller.hpp"
#include "infrastructure/paths.hpp"
#include "media/image_codecs.hpp"
#include "processing/color_analysis.hpp"
#include "processing/effects.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>

namespace ve {
QVariant AppController::handleSettings(Action action, const QVariantList& args) {
    auto setPath = [&](const QString& key, const QString& path) {
        auto result = settings_.set(key, localPath(path));
        if (!result)
            throw std::runtime_error(result.error().toStdString());
        settingsChanged();
    };
    switch (action) {
    case Action::setVfxSetting: {
        const auto key=args.value(0).toString();
        if(!key.startsWith("vfx_")||!settings_.values().contains(key))throw std::runtime_error("Unknown NVIDIA VFX setting.");
        auto value=args.value(1);if(key.endsWith("_path"))value=localPath(value.toString());
        auto result=settings_.set(key,value);if(!result)throw std::runtime_error(result.error().toStdString());
        settingsChanged();return {};
    }
    case Action::setLutFile: {
        const auto path = localPath(args[0].toString());
        if (!path.isEmpty()) {
            const auto lut = CubeLut::load(path);
            (void)lut;
        }
        setPath("lut_path", path);
        return {};
    }
    case Action::setLutReferenceImage:
        setPath("lut_reference_image", args[0].toString());
        if (!settings_.lut_reference_image.isEmpty())
            analyzeLut(false, -1, true);
        return {};
    case Action::setLutAdjustment: {
        const auto key = args[0].toString();
        if (!read(Property::lutAdjustmentValues).toMap().contains(key))
            throw std::runtime_error("Unknown LUT adjustment.");
        const auto value = args[1].toDouble();
        auto result = settings_.set(key, key == "lut_exposure" ? QVariant(std::nearbyint(value * 100) / 100)
                                                               : QVariant(int(std::nearbyint(value))));
        if (!result)
            throw std::runtime_error(result.error().toStdString());
        settingsChanged();
        return {};
    }
    case Action::resetLutAdjustments: {
        const Settings defaults;
        const auto map = defaults.values();
        for (auto it = map.begin(); it != map.end(); ++it)
            if (it.key().startsWith("lut_") &&
                !QStringList{"lut_path", "lut_reference_image", "lut_resolution", "lut_mix"}.contains(
                    it.key())) {
                auto result = settings_.set(it.key(), it.value());
                (void)result;
            }
        settingsChanged();
        return {};
    }
    case Action::autoAdjustLut:
        analyzeLut(false, args.isEmpty() ? -1 : args[0].toDouble());
        return {};
    case Action::autoAdjustLutFixedFrames:
        analyzeLut(true);
        return {};
    case Action::saveLut: {
        if (lutSaving_)
            return {};
        auto path = localPath(args[0].toString());
        if (QFileInfo(path).suffix().compare("cube", Qt::CaseInsensitive) != 0)
            path += ".cube";
        lutSaving_ = true;
        lutSaveStatus_ = "Saving LUT…";
        emit lutSaveChanged();
        const auto settings = settings_;
        const auto control = std::make_shared<JobControl>();
        analysisJob_ = control;
        auto future = QtConcurrent::run(&pool_, [this, path, settings, control] {
            QString error;
            try {
                saveGradedLut(path, settings, *control);
            } catch (const std::exception& e) {
                error = QString::fromUtf8(e.what());
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, error] {
                    lutSaving_ = false;
                    analysisJob_.reset();
                    lutSaveStatus_ = error.isEmpty() ? "Saved LUT: " + path : error;
                    emit lutSaveChanged();
                    emit previewBusyChanged();
                },
                Qt::QueuedConnection);
        });
        (void)future;
        return {};
    }
    case Action::setWorkflowStageEnabled: {
        const auto stage = args[0].toString();
        auto& active = nrMode_ == "Image" ? settings_.image_enabled_stages : settings_.video_enabled_stages;
        const auto& order = nrMode_ == "Image" ? settings_.image_stage_order : settings_.video_stage_order;
        if (!order.contains(stage))
            throw std::runtime_error("Unknown processing card.");
        if (args[1].toBool()) {
            if (!active.contains(stage))
                active.append(stage);
        } else
            active.removeAll(stage);
        const auto enabled=args[1].toBool();
        if(stage=="vfx_vsr")settings_.vfx_vsr_enabled=enabled;
        else if(stage=="vfx_frame_generation")settings_.vfx_frame_generation_enabled=enabled;
        else if(stage=="vfx_upscale")settings_.vfx_upscale_enabled=enabled;
        settingsChanged();
        return {};
    }
    case Action::moveWorkflowStage: {
        auto& order = nrMode_ == "Image" ? settings_.image_stage_order : settings_.video_stage_order;
        const int from = int(order.indexOf(args[0].toString())),
                  to = std::clamp(args[1].toInt(), 0, int(order.size()) - 1);
        if (from >= 0 && from != to) {
            order.move(from, to);
            settingsChanged();
        }
        return {};
    }
    case Action::selectCustomMask: {
        JobControl control;
        const auto path = localPath(args[0].toString());
        const auto image = decodeControlMask(path, control);
        settings_.nr_mask = {{"path", path},
                             {"pixels", image},
                             {"width", image.width()},
                             {"height", image.height()},
                             {"source_name", QFileInfo(path).fileName()},
                             {"revision", QDateTime::currentMSecsSinceEpoch()}};
        settingsChanged();
        return {};
    }
    case Action::clearCustomMask:
        settings_.nr_mask.clear();
        settingsChanged();
        return {};
    case Action::resetToDefaults:
        settings_ = Settings{};
        upscaleMode_ = settings_.upscale_mode;
        settingsChanged();
        saveSettings();
        emit upscaleModeChanged();
        presetStatus_ = "Settings were reset to defaults.";
        emit presetStatusChanged();
        return {};
    case Action::resetTabSettings: {
        const QString tab = args[0].toString();
        if (!idle())
            throw std::runtime_error("Stop the current operation before resetting settings.");
        const Settings defaults;
        const auto values = defaults.values();
        auto candidate = settings_;
        QString prefix;
        if (tab == "upscale")
            prefix = "upscale_";
        else if (tab == "frame-interpolation")
            prefix = "frame_interpolation_";
        if (prefix.isEmpty() && tab != "neural-rendering")
            return {};
        const auto applies = [&](const QString& key) {
            return !prefix.isEmpty()
                       ? key.startsWith(prefix)
                       : !QStringList{"ai_gpu_uuid",
                                                                  "video_gpu_uuid",
                                                                  "ffmpeg_device",
                                                                  "container",
                                                                  "upscale_mode",
                                                                  "upscale_codec",
                                                                  "upscale_container",
                                                                  "upscale_quality",
                                                                  "upscale_preview_length",
                                                                  "upscale_image_output_format",
                                                                  "upscale_image_quality",
                                                                  "upscale_image_rename_mode",
                                                                  "upscale_image_custom_suffix",
                                                                  "frame_interpolation_codec",
                                                                  "frame_interpolation_container",
                                                                  "frame_interpolation_quality",
                                                                  "frame_interpolation_hdr_mode",
                                                                  "frame_interpolation_rename_mode",
                                                                  "frame_interpolation_custom_suffix",
                                                                  "frame_interpolation_preview_length"}
                                                          .contains(key);
        };
#define VE_SETTING(type, name, initial)                                                                      \
    if (applies(QStringLiteral(#name)))                                                                      \
        candidate.name = defaults.name;
#include "configuration/settings_fields.inc"
#undef VE_SETTING
        if (auto valid = candidate.validate(); !valid)
            throw std::runtime_error(valid.error().toStdString());
        settings_ = std::move(candidate);
        upscaleMode_ = settings_.upscale_mode;
        settingsChanged();
        saveSettings();
        emit upscaleModeChanged();
        presetStatus_ = tab + " settings were reset to defaults.";
        emit presetStatusChanged();
        return {};
    }
    case Action::exportPreset:
    case Action::exportPresetTo: {
        const auto name = args[0].toString().trimmed();
        QString path;
        if (action == Action::exportPresetTo)
            path = localPath(args[1].toString());
        else {
            QString stem = name;
            stem.replace(QRegularExpression("[^\\p{L}\\p{N}_-]+"), "_");
            stem = stem.left(80);
            if (stem.isEmpty())
                throw std::runtime_error("Please enter a preset name.");
            path = QDir(Paths::instance().temporary("settings-preset")).filePath(stem + ".json");
        }
        if (auto result = settings_.exportPreset(path, name); !result)
            throw std::runtime_error(result.error().toStdString());
        presetStatus_ = "Exported preset '" + name + "' to " + QFileInfo(path).fileName();
        emit presetStatusChanged();
        return path;
    }
    case Action::importPreset: {
        if (!idle())
            throw std::runtime_error("Stop the current operation before importing a preset.");
        auto result = settings_.importPreset(localPath(args[0].toString()));
        if (!result) {
            presetStatus_ = "Import failed: " + result.error();
            emit presetStatusChanged();
            return false;
        }
        presetStatus_ = "Imported preset '" + *result + "' successfully.";
        upscaleMode_ = settings_.upscale_mode;
        settingsChanged();
        saveSettings();
        emit presetStatusChanged();
        emit upscaleModeChanged();
        return true;
    }
    default:
        throw std::logic_error("Action does not belong to the settings service.");
    }
}
void AppController::analyzeLut(bool fixed, double positionMs, bool matching) {
    if (!idle() || analysisJob_ || context().queue->selectedPath().isEmpty())
        return;
    autoTimer_.stop();
    scrubTimer_.stop();
    const auto input = context().queue->selectedPath();
    const auto media = context().source;
    const auto settings = settings_;
    const double seconds = positionMs >= 0 ? positionMs / 1000 : context().playheadSeconds;
    const auto index = contextIndex();
    const auto generation = context().generation;
    analysisJob_ = std::make_shared<JobControl>();
    const auto control = analysisJob_;
    lutAutoMode_ = matching ? "reference" : fixed ? "fixed" : "frame";
    lutReferenceStatus_ = matching ? "Analyzing input and reference colors…" : QString{};
    emit lutAutoChanged();
    emit lutReferenceChanged();
    emit previewBusyChanged();
    auto future = QtConcurrent::run(&pool_, [this, input, media, settings, seconds, fixed, matching, control,
                                             index, generation] {
        QVariantMap changes;
        QString error;
        try {
            ColorSamples samples;
            size_t frameCount = 1;
            if (media.video && (fixed || matching)) {
                const auto times = fixed ? fixedColorFrameTimes(media.duration, media.fps)
                                         : std::vector<double>{media.duration * .05, media.duration * .2,
                                                               media.duration * .35, media.duration * .5,
                                                               media.duration * .65, media.duration * .8,
                                                               media.duration * .95};
                std::vector<ColorSamples> frames;
                for (const auto time : times) {
                    control->checkpoint();
                    auto sample = imageColorSamples(decodeVideoFrame(input, time, *control), 32000, fixed);
                    if (sample.size() >= 32)
                        frames.push_back(std::move(sample));
                }
                samples = combinedVideoSamples(std::move(frames), &frameCount);
            } else
                samples = imageColorSamples(media.video ? decodeVideoFrame(input, seconds, *control)
                                                        : decodeImage(input, *control));
            if (matching)
                changes = fitReferenceColor(
                    samples, imageColorSamples(decodeImage(settings.lut_reference_image, *control)), settings,
                    *control);
            else
                changes = recommendColor(samples, settings, *control, media.hdr, frameCount);
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, changes, error, index, generation, settings, matching] {
                if (shuttingDown_)
                    return;
                analysisJob_.reset();
                lutAutoMode_.clear();
                bool unchanged = contextIndex() == index && context().generation == generation &&
                                 settings_.lut_path == settings.lut_path &&
                                 settings_.lut_reference_image == settings.lut_reference_image;
                for (const auto& key : read(Property::lutAdjustmentValues).toMap().keys())
                    unchanged = unchanged && settings_.value(key) == settings.value(key);
                if (error.isEmpty() && unchanged) {
                    for (auto it = changes.begin(); it != changes.end(); ++it) {
                        if (matching && it.key() == "lut_mix")
                            continue;
                        auto result = settings_.set(it.key(), it.value());
                        (void)result;
                    }
                    settingsChanged();
                    status_ = matching ? "Reference color adjustments ready"
                                       : "Automatic color adjustments applied.";
                    if (!autoPreview_)
                        preview();
                } else
                    status_ = error.isEmpty()
                                  ? "Color adjustment skipped because the input or color controls changed."
                                  : error;
                if (matching)
                    lutReferenceStatus_ = status_;
                emit lutAutoChanged();
                emit lutReferenceChanged();
                emit previewBusyChanged();
                emit statusMessageChanged();
            },
            Qt::QueuedConnection);
    });
    (void)future;
}
} // namespace ve
