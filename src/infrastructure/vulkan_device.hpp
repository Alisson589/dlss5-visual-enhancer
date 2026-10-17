#pragma once
#include <QString>
#include <QStringList>
#include <QSize>
#include <vector>

namespace ve {
struct VulkanDevice {
    int index = 0;
    QString name, uuid;
    bool discrete = false;
    uint32_t maximumImageDimension2D = 0;
    QStringList extensions;
    [[nodiscard]] QString selection() const { return "vulkan:" + uuid; }
    [[nodiscard]] QString label() const {
        return name + (discrete ? " (Discrete GPU)" : " (Integrated GPU)");
    }
};
[[nodiscard]] const std::vector<VulkanDevice>& vulkanDevices();
[[nodiscard]] std::vector<VulkanDevice> vulkanCandidates(const QString& selection, bool filters = false);
[[nodiscard]] QStringList vulkanArguments(const VulkanDevice& device);
[[nodiscard]] QStringList vulkanFilterArguments(const QString& selection, QSize largestFrame = {});
} // namespace ve
