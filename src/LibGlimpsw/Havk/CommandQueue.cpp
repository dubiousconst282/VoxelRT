#include "Havk.h"

namespace havk {

void CommandList::BeginRendering(const RenderingTarget& targets, bool setViewport) {
    std::vector<VkRenderingAttachmentInfo> attachInfos;
    attachInfos.reserve(targets.Attachments.size() + 2);

    auto PushAttachment = [&](VkRenderingAttachmentInfo const** destListPtr, const AttachmentInfo& info, VkImageAspectFlags aspect) {
        constexpr havk::UseBarrier colorBarrier = { VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        constexpr havk::UseBarrier depthBarrier = { VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT };

        // Don't need to transition layout if we are going to clear target
        if (info.LoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || info.LoadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
            info.Target->CurrentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        Barrier(*info.Target, aspect == VK_IMAGE_ASPECT_COLOR_BIT ? colorBarrier : depthBarrier, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);

        attachInfos.push_back({
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = info.Target->ViewHandle,
            .imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
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
    vkCmdPipelineBarrier(Buffer, image.CurrentStage_, destStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    image.CurrentStage_ = destStage;
    image.CurrentLayout_ = newLayout;
}

void CommandList::Barrier(Image& image, UseBarrier barrier, VkImageLayout layout) {
    VkImageMemoryBarrier vkBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = barrier.Access,
        .oldLayout = image.CurrentLayout_,
        .newLayout = layout == VK_IMAGE_LAYOUT_MAX_ENUM ? image.CurrentLayout_ : layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.Handle,
        .subresourceRange = { 
            .aspectMask = Image::GetAspectMask(image.Desc.Format), 
            .levelCount = image.Desc.NumLevels, 
            .layerCount = image.Desc.NumLayers,
        },
    };
    // TODO: do we even need this?
    if (vkBarrier.oldLayout != vkBarrier.newLayout) {
        vkBarrier.srcAccessMask |= VK_ACCESS_MEMORY_READ_BIT;
    }
    
    vkCmdPipelineBarrier(Buffer, image.CurrentStage_, barrier.Stage, 0, 0, nullptr, 0, nullptr, 1, &vkBarrier);

    image.CurrentStage_ = barrier.Stage;
    image.CurrentLayout_ = vkBarrier.newLayout;
}

void CommandList::Barrier(havk::Buffer& buffer, UseBarrier barrier) {
    VkBufferMemoryBarrier vkBarrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = barrier.Access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer.Handle,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(this->Buffer, buffer.CurrentStage_, barrier.Stage, 0, 0, 0, 1, &vkBarrier, 0, nullptr);

    buffer.CurrentStage_ = barrier.Stage;
}

};