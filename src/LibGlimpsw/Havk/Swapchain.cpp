#include "Havk.h"

namespace havk {

static VkSurfaceFormatKHR SelectSurfaceFormat(VkPhysicalDevice device, VkSurfaceKHR surface) {
    std::vector<VkSurfaceFormatKHR> surfaceFmts;
    uint32_t formatCount;

    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    surfaceFmts.resize(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, surfaceFmts.data());
    
    for (const auto& availableFormat : surfaceFmts) {
        //if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM) {
            return availableFormat;
        }
    }
    return surfaceFmts[0];
}
static VkPresentModeKHR SelectPresentMode(VkPhysicalDevice device, VkSurfaceKHR surface) {
    std::vector<VkPresentModeKHR> presentModes;
    uint32_t presentModeCount;

    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    presentModes.resize(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, presentModes.data());

    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            return mode;
        }
    }
    return presentModes[0];
}

Swapchain::Swapchain(DeviceContext* ctx, GLFWwindow* window, VkSurfaceKHR surface) {
    Context = ctx;
    Window = window;
    Surface = surface;
}

Swapchain::~Swapchain() {
    ReleaseSwapchain();
    vkDestroySurfaceKHR(Context->Instance, Surface, nullptr);
}

// https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/04_Swap_chain_recreation.html
void Swapchain::Initialize() {
    Context->WaitDeviceIdle();

    VkPhysicalDevice device = Context->PhysicalDeviceInfo.Handle;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, Surface, &SurfaceCaps);
    SurfaceFormat = SelectSurfaceFormat(device , Surface);
    PresentMode = SelectPresentMode(device , Surface);

    uint32_t imageCount = SurfaceCaps.minImageCount + 1;
    uint32_t maxImages = SurfaceCaps.maxImageCount;

    if (maxImages > 0 && imageCount > maxImages) {
        imageCount = maxImages;
    }

    VkSwapchainCreateInfoKHR swapchainCI = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = Surface,
        .minImageCount = imageCount,
        .imageFormat = SurfaceFormat.format,
        .imageColorSpace = SurfaceFormat.colorSpace,
        .imageExtent = GetCurrentSurfaceSize(),
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,

        // We're assuming the main queue supports both graphics and present.
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,

        //
        .preTransform = SurfaceCaps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = PresentMode,
        .clipped = VK_TRUE,
    };

    VK_CHECK(vkCreateSwapchainKHR(Context->Device, &swapchainCI, nullptr, &Handle));

    vkGetSwapchainImagesKHR(Context->Device, Handle, &imageCount, nullptr);
    std::vector<VkImage> swcImages(imageCount);
    vkGetSwapchainImagesKHR(Context->Device, Handle, &imageCount, swcImages.data());

    ImageDesc imageDesc = {
        .Format = SurfaceFormat.format,
        .Usage = swapchainCI.imageUsage,
        .Width = swapchainCI.imageExtent.width,
        .Height = swapchainCI.imageExtent.height,
        .NumLayers = 1,
        .NumLevels = 1,
    };

    for (VkImage& image : swcImages) {
        VkImageViewCreateInfo viewCI = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = SurfaceFormat.format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        VkImageView viewHandle;
        VK_CHECK(vkCreateImageView(Context->Device, &viewCI, nullptr, &viewHandle));

        _images.push_back({ .Target = Image::WrapSwapchainImage(Context, image, viewHandle, imageDesc) });
        auto& imageData = _images.back();
        
        VkSemaphoreCreateInfo semaphoreCI = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_CHECK(vkCreateSemaphore(Context->Device, &semaphoreCI, nullptr, &imageData.AvailableSemaphore));
        VK_CHECK(vkCreateSemaphore(Context->Device, &semaphoreCI, nullptr, &imageData.RenderFinishedSemaphore));

        VkFenceCreateInfo fenceCI = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VK_CHECK(vkCreateFence(Context->Device, &fenceCI, nullptr, &imageData.InFlightFence));

        VkCommandBufferAllocateInfo allocCI = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = Context->CmdPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_CHECK(vkAllocateCommandBuffers(Context->Device, &allocCI, &imageData.CmdBuffer));
    }
}

void Swapchain::ReleaseSwapchain() {
    Context->WaitDeviceIdle();

    for (auto& image : _images) {
        vkDestroySemaphore(Context->Device, image.AvailableSemaphore, nullptr);
        vkDestroySemaphore(Context->Device, image.RenderFinishedSemaphore, nullptr);
        vkDestroyFence(Context->Device, image.InFlightFence, nullptr);

        vkFreeCommandBuffers(Context->Device, Context->CmdPool, 1, &image.CmdBuffer);
    }
    _images.clear();

    vkDestroySwapchainKHR(Context->Device, Handle, nullptr);
    Handle = nullptr;
    _currImageIdx = 0;
}

std::pair<Image*, CommandList> Swapchain::AcquireImage() {
    if (Handle == nullptr) {
        Initialize();
    }
    SwcImage& currImage = _images[_currSyncIdx];
    vkWaitForFences(Context->Device, 1, &currImage.InFlightFence, VK_TRUE, UINT64_MAX);
    VkResult acquireResult = vkAcquireNextImageKHR(Context->Device, Handle, UINT64_MAX, currImage.AvailableSemaphore, nullptr, &_currImageIdx);

    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
        ReleaseSwapchain();
        return AcquireImage();
    } else if (acquireResult != VK_SUCCESS) {
        ThrowResult(acquireResult, "Failed to acquire image from swapchain");
    }

    // Only reset the fence if we are submitting work to prevent deadlock
    vkResetFences(Context->Device, 1, &currImage.InFlightFence);

    VkCommandBufferBeginInfo beginCI = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(currImage.CmdBuffer, &beginCI));

    return { _images[_currImageIdx].Target.get(), CommandList(Context, currImage.CmdBuffer) };
}

void Swapchain::Present() {
    SwcImage& currSync = _images[_currSyncIdx];
    ImagePtr& currImage = _images[_currImageIdx].Target;

    auto cmdList = CommandList(Context, currSync.CmdBuffer);
    cmdList.ImageBarrier(*currImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkEndCommandBuffer(currSync.CmdBuffer));

    VkPipelineStageFlags waitMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    (void)Context->Submit(currSync.CmdBuffer, currSync.AvailableSemaphore, waitMask, currSync.RenderFinishedSemaphore, currSync.InFlightFence);

    VkPresentInfoKHR presentInfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &currSync.RenderFinishedSemaphore,
        .swapchainCount = 1,
        .pSwapchains = &Handle,
        .pImageIndices = &_currImageIdx,
    };
    VkResult result = vkQueuePresentKHR(Context->MainQueue, &presentInfo);
    
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        ThrowResult(result, "Failed to present image");
    }
    _currSyncIdx = (_currSyncIdx + 1) % _images.size();
}

VkExtent2D Swapchain::GetCurrentSurfaceSize() {
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(Context->PhysicalDeviceInfo.Handle, Surface, &SurfaceCaps);

    if (SurfaceCaps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return SurfaceCaps.currentExtent;
    }
    int width, height;
    glfwGetFramebufferSize(Window, &width, &height);

    return {
        .width = std::clamp((uint32_t)width, SurfaceCaps.minImageExtent.width, SurfaceCaps.maxImageExtent.width),
        .height = std::clamp((uint32_t)height, SurfaceCaps.minImageExtent.height, SurfaceCaps.maxImageExtent.height),
    };
}

};  // namespace havk