#include "Havk.h"
#include "Internal.h"

#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace havk {

static bool IsSupportedLayer(const char* name) {
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    for (const auto& layerProps : availableLayers) {
        if (strcmp(layerProps.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

static void FindQueueIndices(DeviceInfo& dev, VkSurfaceKHR surface) {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.Handle, &familyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.Handle, &familyCount, queueFamilies.data());

    VkQueueFlags requiredFlags = VK_QUEUE_COMPUTE_BIT | (surface ? VK_QUEUE_GRAPHICS_BIT : 0);

    for (uint32_t i = 0; i < familyCount; i++) {
        VkQueueFlags flags = queueFamilies[i].queueFlags;

        if (surface) {
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev.Handle, i, surface, &presentSupport);
            if (!presentSupport) continue;
        }

        if ((flags & requiredFlags) == requiredFlags) {
            dev.MainQueueIdx = i;
            return;
        }
    }
}
static bool CheckDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*>& requiredExtensions) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::unordered_set<std::string> reqSet(requiredExtensions.begin(), requiredExtensions.end());

    for (const auto& extension : availableExtensions) {
        reqSet.erase(extension.extensionName);
    }
    return reqSet.empty();
}
bool CheckSwapchainSupport(VkPhysicalDevice dev, VkSurfaceKHR surface) {
    uint32_t formatCount;
    uint32_t presentModeCount;

    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface, &formatCount, nullptr);
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface, &presentModeCount, nullptr);

    return formatCount != 0 && presentModeCount != 0;
}

static DeviceInfo SelectPhysicalDevice(VkInstance instance, VkSurfaceKHR surface, const DeviceCreateParams& pars) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    DeviceInfo bestDevice;
    int32_t bestScore = 0;

    for (auto& device : devices) {
        if (!CheckDeviceExtensionSupport(device, pars.RequiredDeviceExtensions)) continue;

        DeviceInfo info = { .Handle = device };
        vkGetPhysicalDeviceProperties(device, &info.Props);
        vkGetPhysicalDeviceFeatures(device, &info.Features);

        FindQueueIndices(info, surface);

        // Skip if no suitable main queue
        if (info.MainQueueIdx == ~0) continue;

        // Skip if user wants surface but no support
        if (surface && !CheckSwapchainSupport(device, surface)) continue;

        int32_t score = 1;

        // Discrete GPUs have a significant performance advantage
        if (info.Props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        }
        if (score > bestScore) {
            bestDevice = std::move(info);
            bestScore = score;
        }
    }

    if (bestDevice.Handle == nullptr) {
        throw std::runtime_error("Could not find suitable Vulkan device");
    }
    return bestDevice;
}
static VkDevice CreateLogicalDevice(const DeviceInfo& devInfo, const DeviceCreateParams& pars) {
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = devInfo.MainQueueIdx,
        .queueCount = 1,
        .pQueuePriorities = &queuePriority,
    };

    VkPhysicalDeviceVulkan13Features vulkan13Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,

        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features vulkan12Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vulkan13Features,

        .uniformAndStorageBuffer8BitAccess = VK_TRUE,
        .shaderFloat16 = VK_TRUE,
        .shaderInt8 = VK_TRUE,

        .descriptorIndexing = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingVariableDescriptorCount = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,

        .scalarBlockLayout = VK_TRUE,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceVulkan11Features vulkan11Features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &vulkan12Features,

        .uniformAndStorageBuffer16BitAccess = VK_TRUE,
        .variablePointersStorageBuffer = VK_TRUE,
        .variablePointers = VK_TRUE,
    };
    VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &vulkan11Features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueCreateInfo,
        .enabledExtensionCount = (uint32_t)pars.RequiredDeviceExtensions.size(),
        .ppEnabledExtensionNames = pars.RequiredDeviceExtensions.data(),
        .pEnabledFeatures = &pars.RequiredFeatures,
    };

    VkDevice device;
    VK_CHECK(vkCreateDevice(devInfo.Handle, &createInfo, nullptr, &device));

    return device;
}

// clang-format off
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* pUserData) {
    // clang-format on
    auto ctx = (DeviceContext*)pUserData;

    LogLevel level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) ? LogLevel::Trace :
                     (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? LogLevel::Warn :
                     (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? LogLevel::Error :
                                                                                    LogLevel::Debug;

    std::string tags = "";

    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) tags += "validation";
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) tags += "perf";
    if (type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) tags += "dev-addr";

    if (!tags.empty()) tags += ": ";

    ctx->Log(level, "%s%s", tags.data(), data->pMessage);

    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        //__builtin_debugtrap();
    }

    return VK_FALSE;
}

static VkSemaphore CreateTimelineSemaphore(VkDevice device) {
    VkSemaphoreTypeCreateInfo timelineCI = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    VkSemaphoreCreateInfo semaphoreCI = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineCI,
        .flags = 0,
    };

    VkSemaphore semaphore;
    vkCreateSemaphore(device, &semaphoreCI, NULL, &semaphore);
    return semaphore;
}

DeviceContextPtr Create(DeviceCreateParams pars) {
    auto ctx = std::make_unique<DeviceContext>();

    if (pars.Window != nullptr) {
        uint32_t count;
        auto glfwExts = glfwGetRequiredInstanceExtensions(&count);
        for (uint32_t i = 0; i < count; i++) {
            pars.RequiredInstanceExtensions.push_back(glfwExts[i]);
        }
    }

    // Layers
    std::vector<const char*> enabledLayers;

    VkDebugUtilsMessengerCreateInfoEXT debugMsgCI = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT,
        .pfnUserCallback = DebugCallback,
        .pUserData = ctx.get(),
    };

    if (pars.EnableDebugLayers && IsSupportedLayer("VK_LAYER_KHRONOS_validation")) {
        enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
        pars.RequiredInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    pars.RequiredFeatures.samplerAnisotropy = VK_TRUE;
    pars.RequiredFeatures.fragmentStoresAndAtomics = VK_TRUE;
    pars.RequiredFeatures.shaderInt16 = VK_TRUE;
    pars.RequiredFeatures.shaderInt64 = VK_TRUE;

    // Instantiation
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = nullptr,
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "havk",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instanceCI = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = (uint32_t)enabledLayers.size(),
        .ppEnabledLayerNames = enabledLayers.data(),
        .enabledExtensionCount = (uint32_t)pars.RequiredInstanceExtensions.size(),
        .ppEnabledExtensionNames = pars.RequiredInstanceExtensions.data(),
    };
    if (pars.EnableDebugLayers) {
        instanceCI.pNext = &debugMsgCI;
    }
    VK_CHECK(vkCreateInstance(&instanceCI, nullptr, &ctx->Instance));

    if (pars.EnableDebugLayers) {
        auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(ctx->Instance, "vkCreateDebugUtilsMessengerEXT");
        func(ctx->Instance, &debugMsgCI, nullptr, &ctx->_debugMessenger);
    }

    VkSurfaceKHR surface = nullptr;
    if (pars.Window != nullptr) {
        VK_CHECK(glfwCreateWindowSurface(ctx->Instance, pars.Window, nullptr, &surface));
        pars.RequiredDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    pars.RequiredDeviceExtensions.push_back(VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME);
    pars.RequiredDeviceExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);


    DeviceInfo devInfo = SelectPhysicalDevice(ctx->Instance, surface, pars);
    ctx->PhysicalDeviceInfo = devInfo;
    ctx->Device = CreateLogicalDevice(devInfo, pars);
    vkGetDeviceQueue(ctx->Device, devInfo.MainQueueIdx, 0, &ctx->MainQueue);

    if (pars.Window != nullptr) {
        ctx->Swapchain = std::make_unique<Swapchain>(ctx.get(), pars.Window, surface);
    }

    VmaAllocatorCreateInfo allocCI = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT | VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT,
        .physicalDevice = ctx->PhysicalDeviceInfo.Handle,
        .device = ctx->Device,
        .instance = ctx->Instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    VK_CHECK(vmaCreateAllocator(&allocCI, &ctx->Allocator));

    VkCommandPoolCreateInfo cmdPoolCI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->PhysicalDeviceInfo.MainQueueIdx,
    };
    VK_CHECK(vkCreateCommandPool(ctx->Device, &cmdPoolCI, nullptr, &ctx->CmdPool));

    ctx->PipeBuilder = std::make_unique<PipelineBuilder>(ctx.get(), pars.ShaderBasePath, pars.EnableShaderHotReload);
    ctx->DescriptorHeap = std::make_unique<DescriptorHeap>(ctx.get());
    ctx->SamplerDescPool = std::make_unique<SamplerDescriptorPool>(ctx.get());

    ctx->QueueSemaphore = CreateTimelineSemaphore(ctx->Device);
    ctx->NextQueueTimestamp = 1;

    return ctx;
}

DeviceContext::~DeviceContext() {
    vkDeviceWaitIdle(Device);

    Tick();

    Swapchain.reset();
    PipeBuilder.reset();
    DescriptorHeap.reset();
    SamplerDescPool.reset();

    vmaDestroyAllocator(Allocator);

    if (_debugMessenger != nullptr) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT");
        func(Instance, _debugMessenger, nullptr);
    }
    if (CmdPool != nullptr) {
        vkDestroyCommandPool(Device, CmdPool, nullptr);
    }
    vkDestroySemaphore(Device, QueueSemaphore, nullptr);

    vkDestroyDevice(Device, nullptr);
    vkDestroyInstance(Instance, nullptr);
}

Future DeviceContext::Submit(VkCommandBuffer cmdBuffer, VkSemaphore waitSemaphore, VkPipelineStageFlags waitMask, VkSemaphore signalSemaphore, VkFence fence) {
    uint64_t finishTimestamp = NextQueueTimestamp++;
    VkSemaphore signals[2] = { QueueSemaphore, signalSemaphore };
    uint64_t waitValues[2] = { finishTimestamp, 0 };

    VkTimelineSemaphoreSubmitInfo timelineInfo = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = signalSemaphore ? 2u : 1,
        .pSignalSemaphoreValues = waitValues,
    };
    VkSubmitInfo submitInfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timelineInfo,
        .waitSemaphoreCount = waitSemaphore ? 1u : 0,
        .pWaitSemaphores = &waitSemaphore,
        .pWaitDstStageMask = &waitMask,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmdBuffer,
        .signalSemaphoreCount = signalSemaphore ? 2u : 1,
        .pSignalSemaphores = signals,
    };
    VK_CHECK(vkQueueSubmit(MainQueue, 1, &submitInfo, fence));

    return Future(this, finishTimestamp);
}

Future DeviceContext::Submit(std::function<void(CommandList)> cb) {
    VkCommandBufferAllocateInfo cmdBufferCI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = CmdPool,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmdBuffer;
    vkAllocateCommandBuffers(Device, &cmdBufferCI, &cmdBuffer);

    VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmdBuffer, &beginInfo);
    cb(CommandList(this, cmdBuffer));
    vkEndCommandBuffer(cmdBuffer);

    Future future = Submit(cmdBuffer, nullptr, 0, nullptr, nullptr);

    struct ManagedCmdList : Resource {
        VkCommandBuffer Handle;

        virtual ~ManagedCmdList() override {
            vkFreeCommandBuffers(Context->Device, Context->CmdPool, 1, &Handle);
        }
    };
    auto res = Resource::make<ManagedCmdList>(this);
    res->Handle = cmdBuffer;
    res->LastUseTimestamp = future.Timestamp;
    res.reset();

    return std::move(future);
}

uint64_t DeviceContext::GetQueueTimestamp() const {
    uint64_t ts;
    vkGetSemaphoreCounterValue(Device, QueueSemaphore, &ts);
    return ts;
}

void Future::Wait(uint64_t timeoutNs) const {
    VkSemaphoreWaitInfo waitInfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &Context->QueueSemaphore,
        .pValues = &Timestamp,
    };
    VK_CHECK(vkWaitSemaphores(Context->Device, &waitInfo, timeoutNs));
}
bool Future::Poll() const {
    return Context->GetQueueTimestamp() >= Timestamp;
}

void DeviceContext::EnqueueDeletion(Resource* ptr) {
    _deletionQueue.push_back(ptr);
}

void DeviceContext::Tick() {
    PipeBuilder->Refresh();

    // MarkUse() sets LastUseTS to NextQueueTimestamp, which is incremented in Submit().
    // Flushing the deletion queue could lead to a delete-while-in-use situation:
    //   MarkUse(cmd1, res1)  ts=3
    //   Submit(cmd2)         ts=3
    //   Submit(cmd3)         ts=4
    //   Submit(cmd1)         ts=5
    //   Tick()               gpu_ts=4  -->  delete(res1)
    //
    // To prevent this, Tick() will record the previous NextQueueTimestamp and delay deletions by one call.
    // TODO
    if (!_deletionQueue.empty()) {
        uint64_t startTimestamp = GetQueueTimestamp();

        std::erase_if(_deletionQueue, [=](Resource* ptr) {
            if (startTimestamp >= ptr->LastUseTimestamp) {
                delete ptr;
                return true;
            }
            return false;
        });
        _prevTickQueueTimestamp = NextQueueTimestamp;
    }
}

void DeviceContext::Log(LogLevel level, const char* message, ...) {
    char buffer[2048];

    va_list args;
    va_start(args, message);
    vsnprintf(buffer, sizeof(buffer), message, args);
    va_end(args);

    const char* levels[] = { "TRACE", "DEBUG", "INFO", "WARN", "ERROR" };

    printf("[havk] %s: %s\n", levels[(int)level], buffer);
    fflush(stdout);
}

void ThrowResult(VkResult result, const char* msg) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(msg) + " failed with code " + std::to_string((int32_t)result));
    }
}

};  // namespace havk