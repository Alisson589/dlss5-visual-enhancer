#include "vulkan_device.hpp"
#include "log.hpp"
#include "paths.hpp"
#include "process.hpp"
#include "windows_runtime.hpp"
#include <QLibrary>
#include <QUuid>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace ve {
namespace {
template <class T> T resolve(QLibrary& library, const char* name) {
    const auto function = reinterpret_cast<T>(library.resolve(name));
    if (!function)
        throw std::runtime_error("The Vulkan driver lacks a required entry point.");
    return function;
}
std::vector<VulkanDevice> enumerate() {
    std::vector<VulkanDevice> result;
    QLibrary library(QString::fromStdWString(windowsSystemLibrary(L"vulkan-1.dll")));
    if (!library.load())
        return result;
    try {
        const auto create = resolve<PFN_vkCreateInstance>(library, "vkCreateInstance");
        const auto destroy = resolve<PFN_vkDestroyInstance>(library, "vkDestroyInstance");
        const auto physicalDevices =
            resolve<PFN_vkEnumeratePhysicalDevices>(library, "vkEnumeratePhysicalDevices");
        const auto properties =
            resolve<PFN_vkGetPhysicalDeviceProperties2>(library, "vkGetPhysicalDeviceProperties2");
        const auto extensions = resolve<PFN_vkEnumerateDeviceExtensionProperties>(
            library, "vkEnumerateDeviceExtensionProperties");
        const VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                            nullptr,
                                            "Visual Enhancer",
                                            1,
                                            "FFmpeg",
                                            1,
                                            VK_API_VERSION_1_1};
        const VkInstanceCreateInfo info{
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &application, 0, nullptr, 0, nullptr};
        VkInstance instance = nullptr;
        if (create(&info, nullptr, &instance) != VK_SUCCESS)
            return result;
        const auto deleter = [destroy](VkInstance value) { destroy(value, nullptr); };
        std::unique_ptr<VkInstance_T, decltype(deleter)> guard(instance, deleter);
        uint32_t count = 0;
        if (physicalDevices(instance, &count, nullptr) != VK_SUCCESS)
            return result;
        std::vector<VkPhysicalDevice> handles(count);
        const auto status = physicalDevices(instance, &count, handles.data());
        if (status != VK_SUCCESS && status != VK_INCOMPLETE)
            return result;
        for (uint32_t index = 0; index < count; ++index) {
            VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
            properties(handles[index], &props);
            if (props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
                continue;
            VulkanDevice device;
            device.index = int(index);
            device.name = QString::fromUtf8(props.properties.deviceName);
            device.discrete = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            device.maximumImageDimension2D = props.properties.limits.maxImageDimension2D;
            device.uuid = QUuid::fromRfc4122(
                              QByteArray(reinterpret_cast<const char*>(identity.deviceUUID), VK_UUID_SIZE))
                              .toString(QUuid::WithoutBraces);
            uint32_t extensionCount = 0;
            extensions(handles[index], nullptr, &extensionCount, nullptr);
            std::vector<VkExtensionProperties> list(extensionCount);
            extensions(handles[index], nullptr, &extensionCount, list.data());
            for (const auto& extension : list)
                device.extensions.append(QString::fromLatin1(extension.extensionName));
            result.push_back(std::move(device));
        }
        std::ranges::stable_sort(
            result, [](const auto& left, const auto& right) { return left.discrete && !right.discrete; });
    } catch (const std::exception& error) {
        Log::instance().append("WARNING", "vulkan", QString::fromUtf8(error.what()));
    }
    return result;
}
} // namespace
const std::vector<VulkanDevice>& vulkanDevices() {
    static const auto devices = enumerate();
    return devices;
}
std::vector<VulkanDevice> vulkanCandidates(const QString& selection, bool filters) {
    if (selection == "cpu" && !filters)
        return {};
    if (selection == "auto" || selection == "cpu")
        return vulkanDevices();
    std::vector<VulkanDevice> result;
    for (const auto& device : vulkanDevices())
        if (device.selection().compare(selection, Qt::CaseInsensitive) == 0)
            result.push_back(device);
    if (result.empty())
        throw std::runtime_error(
            "The selected FFmpeg/Vulkan GPU is unavailable. Choose Automatic or another GPU in Settings.");
    return result;
}
QStringList vulkanArguments(const VulkanDevice& device) {
    return {"-init_hw_device", "vulkan=ve_vk:" + QString::number(device.index), "-filter_hw_device", "ve_vk"};
}
QStringList vulkanFilterArguments(const QString& selection, QSize largestFrame) {
    static std::mutex mutex;
    static std::unordered_map<int, bool> supported;
    for (const auto& device : vulkanCandidates(selection, true)) {
        if (largestFrame.isValid() && (uint32_t(largestFrame.width()) > device.maximumImageDimension2D ||
            uint32_t(largestFrame.height()) > device.maximumImageDimension2D)) continue;
        std::scoped_lock lock(mutex);
        auto found = supported.find(device.index);
        if (found == supported.end()) {
            const auto probe = runProcess(
                Paths::instance().ffmpeg(),
                QStringList{"-v", "error"} + vulkanArguments(device) +
                    QStringList{"-f", "lavfi", "-i", "color=s=64x64:r=1", "-vf",
                                "format=rgba,hwupload,libplacebo=format=rgba,hwdownload,format=rgba",
                                "-frames:v", "1", "-f", "null", "-"});
            found = supported.emplace(device.index, probe.exitCode == 0).first;
        }
        if (found->second)
            return vulkanArguments(device);
    }
    if (largestFrame.isValid())
        throw std::runtime_error(QString("No selected Vulkan GPU supports filtering %1x%2 with its native 2D image limits.")
            .arg(largestFrame.width()).arg(largestFrame.height()).toStdString());
    throw std::runtime_error(
        "Vulkan GPU filtering is unavailable. Install a current AMD, Intel or NVIDIA Vulkan driver.");
}
} // namespace ve
