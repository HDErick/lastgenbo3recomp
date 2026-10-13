// Environment diagnostic only; this program does not load BO3 or emulate Xenos.
#include <switch.h>
#include <vulkan/vulkan.h>
#include <arm_neon.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <sys/stat.h>

namespace {
FILE* log_file;
std::atomic<unsigned> worker_result{0};
void Report(const char* format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    vprintf(format, args);
    if (log_file) { vfprintf(log_file, format, copy); fflush(log_file); }
    va_end(copy);
    va_end(args);
}
void Worker(void*) {
    volatile unsigned value = 7;
    auto vector = vdupq_n_u32(value);
    worker_result.store(vaddvq_u32(vector), std::memory_order_release);
}
void ProbeVulkan() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "BO3 NX environment probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&create, nullptr, &instance);
    Report("vkCreateInstance: %d\n", result);
    if (result != VK_SUCCESS) return;
    uint32_t count = 0;
    result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (result == VK_SUCCESS && count) {
        std::vector<VkPhysicalDevice> devices(count);
        result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result == VK_SUCCESS) for (uint32_t index = 0; index < count; ++index) {
            auto gpu = devices[index];
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(gpu, &props);
            Report("GPU: %s, Vulkan %u.%u.%u, driver 0x%08x\n", props.deviceName,
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
            Report("timestampPeriod (uncalibrated): %.6f ns\n", props.limits.timestampPeriod);
            VkPhysicalDeviceFeatures features{};
            vkGetPhysicalDeviceFeatures(gpu, &features);
            Report("Features: int64=%u geometry=%u tessellation=%u\n",
                features.shaderInt64, features.geometryShader, features.tessellationShader);
            VkPhysicalDeviceMemoryProperties memory{};
            vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
            for (uint32_t h = 0; h < memory.memoryHeapCount; ++h)
                Report("GPU heap %u: %llu MiB flags=0x%x\n", h,
                    static_cast<unsigned long long>(memory.memoryHeaps[h].size >> 20), memory.memoryHeaps[h].flags);
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, queues.data());
            for (uint32_t q = 0; q < families; ++q) {
                Report("Queue family %u: %u queues flags=0x%x\n", q, queues[q].queueCount, queues[q].queueFlags);
                if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                float priority = 1.0f;
                VkDeviceQueueCreateInfo queue{};
                queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                queue.queueFamilyIndex = q;
                queue.queueCount = 1;
                queue.pQueuePriorities = &priority;
                VkDeviceCreateInfo device_info{};
                device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                device_info.queueCreateInfoCount = 1;
                device_info.pQueueCreateInfos = &queue;
                VkDevice device = VK_NULL_HANDLE;
                result = vkCreateDevice(gpu, &device_info, nullptr, &device);
                Report("vkCreateDevice: %d\n", result);
                if (result == VK_SUCCESS) { vkDeviceWaitIdle(device); vkDestroyDevice(device, nullptr); }
                break;
            }
        }
    } else Report("GPU enumeration: %d, count=%u\n", result, count);
    vkDestroyInstance(instance, nullptr);
}
}

int main() {
    consoleInit(nullptr);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/bo3recomp", 0777);
    log_file = fopen("sdmc:/switch/bo3recomp/nx-preflight.log", "w");
    Report("BO3 NX environment probe (not the game)\n");
    Report("Applet type: %u; title takeover required\n", static_cast<unsigned>(appletGetAppletType()));
    Report("System tick frequency: %llu Hz\n", static_cast<unsigned long long>(armGetSystemTickFreq()));
    for (auto type : {InfoType_CoreMask, InfoType_TotalMemorySize, InfoType_UsedMemorySize}) {
        u64 value = 0;
        Result result = svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0);
        Report("svcGetInfo type %u: result=0x%x value=%llu\n", static_cast<unsigned>(type), result,
            static_cast<unsigned long long>(value));
    }
    Report("UsedMemorySize includes the startup heap; it is not malloc usage.\n");
    Thread worker{};
    Result result = threadCreate(&worker, Worker, nullptr, nullptr, 0x8000, 0x3B, -2);
    Report("Worker creation: 0x%x\n", result);
    if (R_SUCCEEDED(result)) {
        result = threadStart(&worker);
        Report("Worker start: 0x%x\n", result);
        if (R_SUCCEEDED(result)) {
            threadWaitForExit(&worker);
            Report("Worker/NEON result: %u (expected 28)\n", worker_result.load(std::memory_order_acquire));
        }
        threadClose(&worker);
    }
    ProbeVulkan();
    Report("Log: sdmc:/switch/bo3recomp/nx-preflight.log\nPress + to exit.\n");
    if (log_file) { fclose(log_file); log_file = nullptr; }
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
    return 0;
}
