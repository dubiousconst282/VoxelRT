#include "Havk.h"
#include "Internal.h"

namespace havk {

void Resource::Release() {
    Context->EnqueueDeletion(this);
}

BufferPtr DeviceContext::CreateBuffer(const BufferDesc& desc) {
    auto buffer = Resource::make<Buffer>(this);
    buffer->Size = desc.Size;
    buffer->Usage = desc.Usage;

    if (desc.Usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
        buffer->Usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    VkBufferCreateInfo bufferCI = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer->Size,
        .usage = buffer->Usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VmaAllocationCreateInfo allocCI = {
        .flags = desc.VmaFlags,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    if (desc.VmaFlags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT)) {
        allocCI.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }

    VK_CHECK(vmaCreateBuffer(Allocator, &bufferCI, &allocCI, &buffer->Handle, &buffer->Allocation, nullptr));

    VmaAllocationInfo allocInfo;
    vmaGetAllocationInfo(Allocator, buffer->Allocation, &allocInfo);

    buffer->MappedData = allocInfo.pMappedData;

    if (buffer->Usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo addressGI = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffer->Handle };
        buffer->DeviceAddress = vkGetBufferDeviceAddress(Device, &addressGI);
    }
    return buffer;
}

Buffer::~Buffer() {
    // 
    vmaDestroyBuffer(Context->Allocator, Handle, Allocation);
}

void Buffer::Write(const void* src, size_t destOffset, size_t byteCount) {
    if (destOffset + byteCount > Size) {
        throw std::range_error("Attempt to write outside buffer bounds");
    }
    memcpy((uint8_t*)MappedData + destOffset, src, byteCount);
    vmaFlushAllocation(Context->Allocator, Allocation, destOffset, byteCount);
}
void Buffer::Invalidate(uint64_t destOffset, uint64_t byteCount) {
    vmaInvalidateAllocation(Context->Allocator, Allocation, destOffset, byteCount);
}
void Buffer::Flush(uint64_t destOffset, uint64_t byteCount) {
    vmaFlushAllocation(Context->Allocator, Allocation, destOffset, byteCount);
}

static VkImageViewType GetViewType(VkImageType type, bool array) {
    switch (type) {
        case VK_IMAGE_TYPE_2D: return array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        case VK_IMAGE_TYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
        default: throw std::logic_error("Image type not supported");
    }
}
static VkImageAspectFlags GetAspectMask(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;

        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;

        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
};
ImagePtr DeviceContext::CreateImage(const ImageDesc& desc) {
    uint32_t maxLevels = (uint32_t)std::bit_width(std::min(std::min(desc.Width, desc.Height), desc.Depth));

    VkImageCreateInfo imageCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = desc.Type,
        .format = desc.Format,
        .extent = { desc.Width, desc.Height, desc.Depth },
        .mipLevels = std::min(desc.NumLevels, maxLevels),
        .arrayLayers = desc.NumLayers,
        .samples = desc.NumSamples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = desc.Usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo allocCI = {
        .usage = VMA_MEMORY_USAGE_AUTO,
        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    };

    auto image = Resource::make<Image>(this);
    image->Desc = desc;
    image->Desc.NumLevels = imageCI.mipLevels; // fixup

    VK_CHECK(vmaCreateImage(Allocator, &imageCI, &allocCI, &image->Handle, &image->Allocation, nullptr));

    VkImageViewCreateInfo viewCI = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image->Handle,
        .viewType = GetViewType(desc.Type, desc.NumLayers >= 2),
        .format = desc.Format,
        .subresourceRange = {
            .aspectMask = GetAspectMask(desc.Format),
            .levelCount = imageCI.mipLevels,
            .layerCount = imageCI.arrayLayers,
        },
    };
    VK_CHECK(vkCreateImageView(Device, &viewCI, nullptr, &image->ViewHandle));

    if (desc.Usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
        image->DescriptorHandle = DescriptorHeap->CreateHandle(image->ViewHandle, desc.Usage);
    }
    return image;
}

Future Image::Upload(const void* data, size_t dataSize, VkRect2D destRect, VkImageSubresourceLayers layers) {
    if (destRect.extent.width == ~0u) destRect.extent.width = Desc.Width;
    if (destRect.extent.height == ~0u) destRect.extent.height = Desc.Height;

    BufferPtr stageBuffer = Context->CreateBuffer({
        .Size = dataSize,
        .Usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .VmaFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
    });
    stageBuffer->Write(data, 0, dataSize);

    return Context->Submit([&](CommandList cmd) {
        bool discardContents = destRect.offset.x == 0 && destRect.offset.y == 0 &&
                               destRect.extent.width == Desc.Width &&
                               destRect.extent.height == Desc.Height;
        cmd.TransitionLayout(*this, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, layers.aspectMask,
                             discardContents);

        VkBufferImageCopy copyRegion = {
            .bufferOffset = 0,
            .bufferRowLength = 0,  // why is this not in bytes?
            .imageSubresource = layers,
            .imageOffset = { destRect.offset.x, destRect.offset.y, 0 },
            .imageExtent = { destRect.extent.width, destRect.extent.height, 1 },
        };
        cmd.MarkUse(*stageBuffer);
        vkCmdCopyBufferToImage(cmd.Buffer, stageBuffer->Handle, Handle, CurrentLayout_, 1, &copyRegion);

        auto naturalLayout = (Desc.Usage & VK_IMAGE_USAGE_STORAGE_BIT) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cmd.TransitionLayout(*this, naturalLayout, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, layers.aspectMask);
    });
}

ImagePtr Image::WrapSwapchainImage(DeviceContext* ctx, VkImage handle, VkImageView viewHandle, ImageDesc desc) {
    auto image = Resource::make<Image>(ctx);
    image->Context = ctx;
    image->Desc = desc;
    image->Handle = handle;
    image->ViewHandle = viewHandle;
    return image;
}
void Image::Release() {
    if (Allocation != nullptr) {
        Resource::Release();
    } else {
        delete this;
    }
}

Image::~Image() {
    if (DescriptorHandle != InvalidHandle) {
        Context->DescriptorHeap->DestroyHandle(DescriptorHandle);
    }

    vkDestroyImageView(Context->Device, ViewHandle, nullptr);

    if (Allocation != nullptr) {
        vmaDestroyImage(Context->Allocator, Handle, Allocation);
    }
}

DescriptorHeap::DescriptorHeap(DeviceContext* ctx) {
    Context = ctx;

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, Capacity },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, Capacity },
    };
    VkDescriptorPoolCreateInfo poolCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = poolSizes,
    };
    VK_CHECK(vkCreateDescriptorPool(Context->Device, &poolCI, nullptr, &Pool));

    const VkDescriptorBindingFlags flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | 
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;

    VkDescriptorBindingFlags bindingFlags[2] { flags, flags };

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 2,
        .pBindingFlags = bindingFlags,
    };

    // Variable size descriptor bindings seem to be pointless, we'll just allocate a fixed-size pool instead
    // TODO: https://docs.vulkan.org/spec/latest/proposals/proposals/VK_EXT_mutable_descriptor_type.html

    VkDescriptorSetLayoutBinding bindings[2];

    for (uint32_t i = 0; i < 2; i++) {
        bindings[i] = {
            .binding = i,
            .descriptorType = poolSizes[i].type,
            .descriptorCount = poolSizes[i].descriptorCount,
            .stageFlags = VK_SHADER_STAGE_ALL,
        };
    }
    VkDescriptorSetLayoutCreateInfo layoutCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &bindingFlagsCI,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    VK_CHECK(vkCreateDescriptorSetLayout(Context->Device, &layoutCI, nullptr, &SetLayout));

    VkDescriptorSetAllocateInfo allocCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = Pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &SetLayout,
    };
    VK_CHECK(vkAllocateDescriptorSets(Context->Device, &allocCI, &Set));
}
DescriptorHeap::~DescriptorHeap() {
    vkDestroyDescriptorSetLayout(Context->Device, SetLayout, nullptr);
    vkDestroyDescriptorPool(Context->Device, Pool, nullptr);
}

uint32_t DescriptorHeap::CreateHandle(VkImageView viewHandle, VkImageUsageFlags usage) {
    uint32_t handle = _allocator.Alloc();
    if (handle == ~0u) {
        throw std::runtime_error("Descriptor heap is full");
    }
    VkDescriptorImageInfo imageInfo = {
        .imageView = viewHandle
    };
    VkWriteDescriptorSet writeReq = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = Set,
        .dstArrayElement = handle,
        .descriptorCount = 1,
        .pImageInfo = &imageInfo,
    };
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        imageInfo.imageLayout = (usage & VK_IMAGE_USAGE_STORAGE_BIT) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writeReq.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writeReq.dstBinding = 0;
        vkUpdateDescriptorSets(Context->Device, 1, &writeReq, 0, nullptr);
    }
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writeReq.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writeReq.dstBinding = 1;
        vkUpdateDescriptorSets(Context->Device, 1, &writeReq, 0, nullptr);
    }
    return handle;
}
void DescriptorHeap::DestroyHandle(uint32_t handle) {
    _allocator.Free(handle);
    // Nothing else to do, spec says "descriptors become undefined after underlying resources are destroyed".
}

uint32_t DescriptorHeap::HandleAllocator::Alloc() {
    for (uint32_t i = 0; i < std::size(UsedMap); i++) {
        uint32_t wi = (i + NextFreeWordIdxHint) % std::size(UsedMap);
        if (UsedMap[wi] != ~0ull) {
            uint32_t j = (uint32_t)std::countr_one(UsedMap[wi]);
            UsedMap[wi] |= 1ull << j;
            NextFreeWordIdxHint = wi;
            
            return wi * 64 + j;
        }
    }
    return ~0u;
}
void DescriptorHeap::HandleAllocator::Free(uint32_t addr) {
    UsedMap[addr / 64] &= ~(1ull << (addr & 63));
}

};  // namespace havk