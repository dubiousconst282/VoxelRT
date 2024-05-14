#pragma once

#include <OGL/QuickGL.h>
#include <OGL/ShaderLib.h>
#include <Common/Camera.h>

struct GBuffer {
    enum class DebugChannel { None, Albedo, Irradiance, Normals, TraversalIters, Variance };

    std::shared_ptr<ogl::Shader> ReprojShader, FilterShader, PresentShader;

    std::unique_ptr<ogl::Texture2D> AlbedoTex, PrevAlbedoTex;
    std::unique_ptr<ogl::Texture2D> IrradianceTex, PrevIrradianceTex, TempIrradianceTex;
    std::unique_ptr<ogl::Texture2D> DepthTex, PrevDepthTex;
    std::unique_ptr<ogl::Texture2D> MomentsTex, PrevMomentsTex;
    std::unique_ptr<ogl::Texture2D> HistoryLenTex;

    glm::mat4 CurrentProj, HistoryProj;
    glm::dvec3 CurrentPos, HistoryPos;
    uint32_t FrameNo = 0;

    DebugChannel DebugChannelView = DebugChannel::None;
    uint32_t NumDenoiserPasses = 5;

    GBuffer(ogl::ShaderLib& shlib) {
        ReprojShader = shlib.LoadComp("Denoise/Reproject");
        FilterShader = shlib.LoadComp("Denoise/Filter");
        PresentShader = shlib.LoadFrag("GBufferBlit");
    }

    void SetCamera(glim::Camera& cam, glm::ivec2 viewSize, bool resetHistory) {
        if (AlbedoTex == nullptr || AlbedoTex->Width != viewSize.x || AlbedoTex->Height != viewSize.y) {
            AlbedoTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA8);
            PrevAlbedoTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA8);

            IrradianceTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA16F);
            PrevIrradianceTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA16F);
            TempIrradianceTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA16F);

            DepthTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_R32F);
            PrevDepthTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_R32F);

            MomentsTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RG16F);
            PrevMomentsTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RG16F);

            HistoryLenTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_R8UI);
        }
        HistoryPos = CurrentPos;
        HistoryProj = CurrentProj;

        CurrentPos = cam.ViewPosition;
        CurrentProj = cam.GetProjMatrix() * cam.GetViewMatrix(false);

        std::swap(AlbedoTex, PrevAlbedoTex);
        std::swap(DepthTex, PrevDepthTex);
        std::swap(MomentsTex, PrevMomentsTex);
        FrameNo++;

        ReprojShader->SetUniform("u_ForceResetHistory", resetHistory);
    }
    void SetUniforms(ogl::Shader& shader) {
        shader.SetUniform("u_AlbedoNormalTex", *AlbedoTex);
        shader.SetUniform("u_IrradianceTex", *IrradianceTex);
        shader.SetUniform("u_DepthTex", *DepthTex);
        shader.SetUniform("u_MomentsTex", *MomentsTex);

        shader.SetUniform("u_PrevAlbedoNormalTex", *PrevAlbedoTex);
        shader.SetUniform("u_PrevIrradianceTex", *PrevIrradianceTex);
        shader.SetUniform("u_PrevDepthTex", *PrevDepthTex);
        shader.SetUniform("u_PrevMomentsTex", *PrevMomentsTex);

        shader.SetUniform("u_HistoryLenTex", *HistoryLenTex);

        glm::ivec2 viewSize = glm::ivec2(AlbedoTex->Width, AlbedoTex->Height);
        shader.SetUniform("u_ProjMat", CurrentProj);
        shader.SetUniform("u_InvProjMat", GetInverseProjScreenMat(CurrentProj, viewSize));
        shader.SetUniform("u_HistoryProjMat", HistoryProj);
        shader.SetUniform("u_HistoryInvProjMat", GetInverseProjScreenMat(HistoryProj, viewSize));
        shader.SetUniform("u_OriginFrac", glm::vec3(glm::fract(CurrentPos)));
        shader.SetUniform("u_HistoryOriginFrac", glm::vec3(glm::fract(HistoryPos)));
        shader.SetUniform("u_OriginDelta", glm::vec3(CurrentPos - HistoryPos));
        shader.SetUniform("u_FrameNo", (int)FrameNo);
        shader.SetUniform("u_DebugChannel", (int)DebugChannelView);
    }

    void DenoiseAndPresent() {
        uint32_t groupsX = (AlbedoTex->Width + 7) / 8;
        uint32_t groupsY = (AlbedoTex->Height + 7) / 8;

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
        }

        // Blit to screen
        SetUniforms(*PresentShader);
        PresentShader->DispatchFullscreen();

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