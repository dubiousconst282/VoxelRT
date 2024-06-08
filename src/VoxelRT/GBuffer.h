#pragma once

#include <Havk/Havk.h>
#include <Common/Camera.h>
#include <vulkan/vulkan_core.h>

struct GBufferUniforms {
    havk::ImageHandle AlbedoTex, PrevAlbedoTex;
    havk::ImageHandle IrradianceTex, PrevIrradianceTex;
    havk::ImageHandle DepthTex, PrevDepthTex;
    havk::ImageHandle MomentsTex, PrevMomentsTex;
    havk::ImageHandle HistoryLenTex;

    glm::mat4 ProjMat, InvProjMat;
    glm::mat4 HistoryProjMat, HistoryInvProjMat;
    glm::vec3 OriginFrac, HistoryOriginFrac;
    glm::vec3 OriginDelta;
    uint32_t FrameNo;
};

struct GBuffer {
    enum class DebugChannel { None, Albedo, Irradiance, Normals, TraversalIters, Variance };

    havk::DeviceContext* Context;
    havk::ComputePipelinePtr ReprojShader, FilterShader;
    havk::GraphicsPipelinePtr PresentShader;

    havk::ImagePtr AlbedoTex, PrevAlbedoTex;
    havk::ImagePtr IrradianceTex, PrevIrradianceTex, TempIrradianceTex;
    havk::ImagePtr DepthTex, PrevDepthTex;
    havk::ImagePtr MomentsTex, PrevMomentsTex;
    havk::ImagePtr HistoryLenTex;

    havk::BufferPtr UniformBuffer;

    glm::mat4 CurrentProj, HistoryProj;
    glm::dvec3 CurrentPos, HistoryPos;
    uint32_t FrameNo = 0;

    DebugChannel DebugChannelView = DebugChannel::None;
    uint32_t NumDenoiserPasses = 5;

    glm::uvec2 RenderSize = {};
    float _renderScale = 1.0f;

    GBuffer(havk::DeviceContext* ctx) {
        Context = ctx;

        //ReprojShader = ctx->PipeBuilder->CreateCompute("Denoise/Reproject.slang");
        // FilterShader = ctx->PipeBuilder->CreateCompute("Denoise/Filter.slang");
        PresentShader = ctx->PipeBuilder->CreateGraphics("GBufferBlit.slang", {
            .EnableDepthTest = false,
            .EnableDepthWrite = false,
            .OutputFormats = { ctx->Swapchain->SurfaceFormat.format },
        });
        UniformBuffer = ctx->CreateBuffer({
            .Size = sizeof(GBufferUniforms),
            .Usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
        });
    }

    void SetCamera(havk::CommandList& cmds, glim::Camera& cam, glm::uvec2 renderSize, bool resetHistory) {
        if (AlbedoTex == nullptr || RenderSize != renderSize) {
            RenderSize = renderSize;

            const auto CreateImage = [&](VkFormat format) {
                return Context->CreateImage({
                    .Format = format,
                    .Usage = VK_IMAGE_USAGE_STORAGE_BIT,
                    .Width = renderSize.x,
                    .Height = renderSize.y,
                    .NumLevels = 1,
                });
            };

            AlbedoTex = CreateImage(VK_FORMAT_R8G8B8A8_UNORM);
            PrevAlbedoTex = CreateImage(VK_FORMAT_R8G8B8A8_UNORM);

            IrradianceTex = CreateImage(VK_FORMAT_R16G16B16A16_SFLOAT);
            PrevIrradianceTex = CreateImage(VK_FORMAT_R16G16B16A16_SFLOAT);
            TempIrradianceTex = CreateImage(VK_FORMAT_R16G16B16A16_SFLOAT);

            DepthTex = CreateImage(VK_FORMAT_R32_SFLOAT);
            PrevDepthTex = CreateImage(VK_FORMAT_R32_SFLOAT);

            MomentsTex = CreateImage(VK_FORMAT_R16G16_SFLOAT);
            PrevMomentsTex = CreateImage(VK_FORMAT_R16G16_SFLOAT);

            HistoryLenTex = CreateImage(VK_FORMAT_R8_UINT);
        }
        HistoryPos = CurrentPos;
        HistoryProj = CurrentProj;

        CurrentPos = cam.ViewPosition;
        CurrentProj = cam.GetProjMatrix() * cam.GetViewMatrix(false);

        std::swap(AlbedoTex, PrevAlbedoTex);
        std::swap(DepthTex, PrevDepthTex);
        std::swap(MomentsTex, PrevMomentsTex);
        FrameNo++;

        // ReprojShader->SetUniform("u_ForceResetHistory", resetHistory);
        WriteUniforms(cmds, *UniformBuffer);
    }

    void WriteUniforms(havk::CommandList& cmds, havk::Buffer& buffer) {
        GBufferUniforms u;

        u.AlbedoTex = AlbedoTex->DescriptorHandle;
        u.IrradianceTex = IrradianceTex->DescriptorHandle;
        u.DepthTex = DepthTex->DescriptorHandle;
        u.MomentsTex = MomentsTex->DescriptorHandle;
        u.HistoryLenTex = HistoryLenTex->DescriptorHandle;

        u.PrevAlbedoTex = PrevAlbedoTex->DescriptorHandle;
        u.PrevIrradianceTex = PrevIrradianceTex->DescriptorHandle;
        u.PrevDepthTex = PrevDepthTex->DescriptorHandle;
        u.PrevMomentsTex = PrevMomentsTex->DescriptorHandle;

        glm::ivec2 viewSize = glm::ivec2(AlbedoTex->Desc.Width, AlbedoTex->Desc.Height);
        u.ProjMat = CurrentProj;
        u.InvProjMat = GetInverseProjScreenMat(CurrentProj, viewSize);
        u.HistoryProjMat = HistoryProj;
        u.HistoryInvProjMat = GetInverseProjScreenMat(HistoryProj, viewSize);
        u.OriginFrac = glm::vec3(glm::fract(CurrentPos));
        u.HistoryOriginFrac = glm::vec3(glm::fract(HistoryPos));
        u.OriginDelta = glm::vec3(CurrentPos - HistoryPos);
        u.FrameNo = FrameNo;

        cmds.UpdateBuffer(buffer, 0, sizeof(GBufferUniforms), &u);
    }

    void Resolve(havk::Image* target, havk::CommandList& cmds) {
        uint32_t groupsX = (RenderSize.x + 7) / 8;
        uint32_t groupsY = (RenderSize.y + 7) / 8;
/*
        if (DebugChannelView != DebugChannel::TraversalIters) {
            SetUniforms(*ReprojShader);
            ReprojShader->DispatchCompute(groupsX, groupsY, 1);

            if (NumDenoiserPasses > 0) {
                // Variance estimation
                SetUniforms(*FilterShader);
                FilterShader->SetUniform("u_PassNo", -1);
                FilterShader->SetUniform("u_TempIrradianceTex", *TempIrradianceTex);
                FilterShader->DispatchCompute(groupsX, groupsY, 1);

                // A-trous filter
                for (int32_t i = 0; i < NumDenoiserPasses; i++) {
                    ogl::Texture2D& inputTex = i == 1 ? *PrevIrradianceTex : (i % 2 == 0 ? *TempIrradianceTex : *IrradianceTex);
                    ogl::Texture2D& outputTex = i % 2 == 0 ? *IrradianceTex : *TempIrradianceTex;

                    FilterShader->SetUniform("u_PassNo", i);
                    FilterShader->SetUniform("u_TempIrradianceTex", inputTex);
                    FilterShader->SetUniform("u_IrradianceTex", outputTex);
                    FilterShader->DispatchCompute(groupsX, groupsY, 1);

                    // Save output from first iteration as the history for the next frame
                    if (i == 0) {
                        std::swap(PrevIrradianceTex, IrradianceTex);
                    }
                }
                // FIXME: lag when NumPasses == 2
                if (NumDenoiserPasses % 2 != 0) {
                    std::swap(TempIrradianceTex, IrradianceTex);
                }
            }
        }*/

        // Blit to screen
        struct PresentConstants {
            VkDeviceAddress GBuffer;
            DebugChannel Channel;
        };
        PresentConstants pc = {
            .GBuffer = cmds.GetDeviceAddress(*UniformBuffer, havk::UseBarrier::GraphicsRead),
            .Channel = DebugChannelView,
        };
        cmds.GetDescriptorHandle(*AlbedoTex, havk::UseBarrier::GraphicsRead);

        cmds.BeginRendering({ .Attachments = { { .Target = target, .LoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE } } }, true);
        PresentShader->Draw(cmds, { .NumVertices = 3 }, pc);
        cmds.EndRendering();

        if (NumDenoiserPasses == 0) {
            std::swap(PrevIrradianceTex, IrradianceTex);
        }
    }

    // Computes inverse projection matrix, scaled to take coordinates in range [0..viewSize] rather than [-1..1]
    static glm::mat4 GetInverseProjScreenMat(const glm::mat4& mat, glm::ivec2 viewSize) {
        glm::mat4 invProj = glm::inverse(mat);
        invProj = glm::translate(invProj, glm::vec3(-1.0f, -1.0f, 0.0f));
        invProj = glm::scale(invProj, glm::vec3(2.0f / viewSize.x, 2.0f / viewSize.y, 1.0f));
        invProj = glm::translate(invProj, glm::vec3(0.5f, 0.5f, 0.0f)); // offset to pixel center
        return invProj;
    }

    // Temporal anti-alias sub-pixel jitter offsets - Halton(2, 3)
    static inline const glm::vec2 Halton23[16] = {
        { 0.50000, 0.33333 }, { 0.25000, 0.66667 }, { 0.75000, 0.11111 }, { 0.12500, 0.44444 },  //
        { 0.62500, 0.77778 }, { 0.37500, 0.22222 }, { 0.87500, 0.55556 }, { 0.06250, 0.88889 },  //
        { 0.56250, 0.03704 }, { 0.31250, 0.37037 }, { 0.81250, 0.70370 }, { 0.18750, 0.14815 },  //
        { 0.68750, 0.48148 }, { 0.43750, 0.81481 }, { 0.93750, 0.25926 }, { 0.03125, 0.59259 },
    };
};