#pragma once
#include <QString>
#include <QStringList>
#include <vector>

namespace ve {
struct VulkanDevice {
    int index = 0;
    QString name, uuid;
    bool discrete = false;
    QStringList extensions;
    [[nodiscard]] QString selection() const { return "vulkan:" + uuid; }
    [[nodiscard]] QString label() const {
        return name + (discrete ? " (Discrete GPU)" : " (Integrated GPU)");
    }
};
[[nodiscard]] const std::vector<VulkanDevice>& vulkanDevices();
[[nodiscard]] std::vector<VulkanDevice> vulkanCandidates(const QString& selection, bool filters = false);
[[nodiscard]] QStringList vulkanArguments(const VulkanDevice& device);
[[nodiscard]] QStringList vulkanFilterArguments(const QString& selection);
} // namespace ve
