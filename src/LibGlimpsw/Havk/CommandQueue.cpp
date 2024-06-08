#include "Havk.h"

namespace havk {

void CommandList::BeginRendering(const RenderingTarget& targets, bool setViewport) {
    std::vector<VkRenderingAttachmentInfo> attachInfos;
    attachInfos.reserve(targets.Attachments.size() + 2);

    auto PushAttachment = [&](VkRenderingAttachmentInfo const** destListPtr, const AttachmentInfo& info, VkImageAspectFlags aspect) {
        TransitionLayout(*info.Target, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, aspect,
                         info.LoadOp != VK_ATTACHMENT_LOAD_OP_CLEAR);

        attachInfos.push_back({
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = info.Target->ViewHandle,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = info.LoadOp,
            .storeOp = info.StoreOp,
            .clearValue = info.ClearValue,
        });
        
        if (*destListPtr == nullptr) {
            *destListPtr = &attachInfos.back();
        }
    };

    VkRenderingInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = targets.Region,
        .layerCount = 1,
    };
    info.colorAttachmentCount = (uint32_t)targets.Attachments.size();

    for (auto& attach : targets.Attachments) {
        PushAttachment(&info.pColorAttachments, attach, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (targets.DepthAttachment.Target) {
        PushAttachment(&info.pDepthAttachment, targets.DepthAttachment, VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (targets.StencilAttachment.Target) {
        PushAttachment(&info.pStencilAttachment, targets.StencilAttachment, VK_IMAGE_ASPECT_STENCIL_BIT);
    }

    auto& mainAttach = targets.Attachments.size() > 0 ? targets.Attachments[0] :
                   targets.DepthAttachment.Target ? targets.DepthAttachment :
                                                    targets.StencilAttachment;

    if (targets.Region.extent.width == 0 || targets.Region.extent.height == 0) {
        info.renderArea.extent = { mainAttach.Target->Desc.Width, mainAttach.Target->Desc.Height };
    }
    vkCmdBeginRendering(Buffer, &info);

    if (setViewport) {
        SetViewport({ 0, 0, (float)mainAttach.Target->Desc.Width, (float)mainAttach.Target->Desc.Height, 0, +1 });
        SetScissor({ 0, 0, mainAttach.Target->Desc.Width, mainAttach.Target->Desc.Height });
    }
}

void CommandList::TransitionLayout(Image& image, VkImageLayout newLayout, VkPipelineStageFlags destStage,
                                   VkImageAspectFlags aspect, bool discardContents) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = discardContents ? VK_IMAGE_LAYOUT_UNDEFINED : image.CurrentLayout_,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.Handle,
        .subresourceRange = { .aspectMask = aspect, .levelCount = image.Desc.NumLevels, .layerCount = image.Desc.NumLayers },
    };
    vkCmdPipelineBarrier(Buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, destStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    image.CurrentLayout_ = newLayout;
    MarkUse(image);
}

ImageHandle CommandList::GetDescriptorHandle(Image& image, UseBarrier barrier, VkImageLayout layout) {
    VkImageMemoryBarrier vkBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = barrier.Access,
        .oldLayout = image.CurrentLayout_,
        .newLayout = layout == VK_IMAGE_LAYOUT_MAX_ENUM ? image.CurrentLayout_ : layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.Handle,
        // TODO: handle this annoying aspect thingy
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = image.Desc.NumLevels, .layerCount = image.Desc.NumLayers },
    };
    vkCmdPipelineBarrier(Buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, barrier.Stage, 0, 0, nullptr, 0, nullptr, 1, &vkBarrier);

    image.CurrentLayout_ = vkBarrier.newLayout;
    MarkUse(image);

    return image.DescriptorHandle;
}

VkDeviceAddress CommandList::GetDeviceAddress(havk::Buffer& buffer, UseBarrier barrier) {
    VkBufferMemoryBarrier vkBarrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = barrier.Access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer.Handle,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(this->Buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, barrier.Stage, 0, 0, 0, 1, &vkBarrier, 0, nullptr);

    MarkUse(buffer);

    return buffer.DeviceAddress;
}

};