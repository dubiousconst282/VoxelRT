#pragma once

#include <OGL/QuickGL.h>
#include <OGL/ShaderLib.h>

struct BlueNoise {
    std::unique_ptr<ogl::Texture2D> ScramblingTex, SobolTex;

    BlueNoise() {
        ScramblingTex = ogl::Texture2D::Load("assets/bluenoise/ScramblingTile_128x128x4_1spp.png", 1, GL_RGBA8UI);
        SobolTex = ogl::Texture2D::Load("assets/bluenoise/Sobol_256x256.png", 1, GL_RGBA8UI);
    }

    void SetUniforms(ogl::Shader& shader) {
        shader.SetUniform("u_BlueNoiseScramblingTex", *ScramblingTex);
        shader.SetUniform("u_BlueNoiseSobolTex", *SobolTex);
    }
};