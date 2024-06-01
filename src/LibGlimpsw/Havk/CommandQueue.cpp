#include "Havk.h"

namespace havk {

void CommandList::BeginRendering(const RenderingTarget& targets) {
    std::vector<VkRenderingAttachmentInfo> attachInfos;
    attachInfos.reserve(targets.Attachments.size() + 2);

    auto PushAttachment = [&](VkRenderingAttachmentInfo const** destListPtr, const AttachmentInfo& info, VkImageAspectFlags aspect) {
        auto access = aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                                          : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        ImageBarrier(*info.Target, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL, access, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, aspect,
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

    if (targets.Region.extent.width == 0 || targets.Region.extent.height == 0) {
        auto& attach = targets.Attachments.size() > 0 ? targets.Attachments[0] :
                       targets.DepthAttachment.Target ? targets.DepthAttachment :
                                                        targets.StencilAttachment;

        info.renderArea.extent = { attach.Target->Desc.Width, attach.Target->Desc.Height };
    }
    vkCmdBeginRendering(Buffer, &info);
}

void CommandList::ImageBarrier(Image& image, VkImageLayout newLayout, VkAccessFlags newAccess, VkPipelineStageFlags newStage,
                               VkImageAspectFlags aspect, bool discard) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = image.CurrentAccess_,
        .dstAccessMask = newAccess,
        .oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : image.CurrentLayout_,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.Handle,
        .subresourceRange = { .aspectMask = aspect, .levelCount = image.Desc.NumLevels, .layerCount = image.Desc.NumLayers },
    };
    vkCmdPipelineBarrier(Buffer, image.CurrentStage_, newStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    image.CurrentLayout_ = newLayout;
    image.CurrentAccess_ = newAccess;
    image.CurrentStage_ = newStage;
}
};