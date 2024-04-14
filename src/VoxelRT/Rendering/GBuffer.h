#pragma once

#include <OGL/QuickGL.h>
#include <OGL/ShaderLib.h>
#include <Common/Camera.h>

struct GBuffer {
    std::shared_ptr<ogl::Shader> DenoiseShader, PresentShader;
    std::unique_ptr<ogl::Texture2D> HistoryTex, CurrentTex;

    glm::mat4 CurrentProj, CurrentInvProj, HistoryProj;
    glm::dvec3 CurrentPos, HistoryPos;
    uint32_t FrameNo = 0;

    GBuffer(ogl::ShaderLib& shlib) {
        DenoiseShader = shlib.LoadComp("Denoise");
        PresentShader = shlib.LoadFrag("GBufferBlit");
    }

    void SetCamera(glim::Camera& cam, glm::ivec2 viewSize, bool resetHistory) {
        if (CurrentTex == nullptr || CurrentTex->Width != viewSize.x || CurrentTex->Height != viewSize.y) {
            CurrentTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA32UI);
            HistoryTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA32UI);
        }
        HistoryPos = CurrentPos;
        HistoryProj = CurrentProj;

        CurrentPos = cam.ViewPosition;
        CurrentProj = cam.GetProjMatrix() * cam.GetViewMatrix(false);
        CurrentInvProj = glm::inverse(CurrentProj);

        if (!resetHistory) {
            std::swap(CurrentTex, HistoryTex);
            FrameNo++;
        } else {
            FrameNo = 0;
        }
    }
    void SetUniforms(ogl::Shader& shader) {
        shader.SetUniform("u_GBuffer", *CurrentTex);
        shader.SetUniform("u_HistoryBuffer", *HistoryTex);

        shader.SetUniform("u_InvProjMat", CurrentInvProj);
        shader.SetUniform("u_OriginFrac", glm::vec3(glm::fract(CurrentPos)));
        shader.SetUniform("u_FrameNo", (int)FrameNo);
    }

    void DenoiseAndPresent() {
        DenoiseShader->SetUniform("u_HistoryProjMat", HistoryProj);
        DenoiseShader->SetUniform("u_OriginDelta", glm::vec3(glm::floor(CurrentPos) - HistoryPos));
        SetUniforms(*DenoiseShader);

        uint32_t groupsX = (CurrentTex->Width + 7) / 8;
        uint32_t groupsY = (CurrentTex->Height + 7) / 8;
        DenoiseShader->DispatchCompute(groupsX, groupsY, 1);
 
        // Blit to screen
        PresentShader->SetUniform("u_GBuffer", *CurrentTex);
        PresentShader->DispatchFullscreen();
    }

    // Temporal anti-alias sub-pixel jitter offsets - Halton(2, 3)
    static inline const glm::vec2 Halton23[16] = {
        { 0.50000, 0.33333 }, { 0.25000, 0.66667 }, { 0.75000, 0.11111 }, { 0.12500, 0.44444 },  //
        { 0.62500, 0.77778 }, { 0.37500, 0.22222 }, { 0.87500, 0.55556 }, { 0.06250, 0.88889 },  //
        { 0.56250, 0.03704 }, { 0.31250, 0.37037 }, { 0.81250, 0.70370 }, { 0.18750, 0.14815 },  //
        { 0.68750, 0.48148 }, { 0.43750, 0.81481 }, { 0.93750, 0.25926 }, { 0.03125, 0.59259 },
    };
};