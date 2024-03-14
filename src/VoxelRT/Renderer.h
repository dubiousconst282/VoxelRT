#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <imgui.h>
#include <Common/Camera.h>
#include <Common/SettingStore.h>

#include <OGL/QuickGL.h>
#include <OGL/ShaderLib.h>

#include "VoxelMap.h"

struct Renderer {
    virtual ~Renderer() {}

    virtual void RenderFrame(glim::Camera& cam, glm::uvec2 viewSize) = 0;
    virtual void DrawSettings(glim::SettingStore& settings) {}
};

struct GpuVoxelStorage;
struct FlatVoxelStorage;

struct GpuRenderer : public Renderer {
    GpuRenderer(ogl::ShaderLib& shlib, std::shared_ptr<VoxelMap> map);
    ~GpuRenderer();

    virtual void RenderFrame(glim::Camera& cam, glm::uvec2 viewSize);
    virtual void DrawSettings(glim::SettingStore& settings);

private:
    std::shared_ptr<VoxelMap> _map;
    std::unique_ptr<GpuVoxelStorage> _storage;

    std::shared_ptr<ogl::Shader> _mainShader;
    bool _showHeatmap;
    bool _useAnisotropicLods;

    glim::TimeStat _frameTime;
    GLuint _frameQueryObj = 0;
    std::unique_ptr<ogl::Buffer> _metricsBuffer;
};
struct CpuRenderer : public Renderer {
    CpuRenderer(ogl::ShaderLib& shlib, std::shared_ptr<VoxelMap> map);
    ~CpuRenderer();

#if SIMD_AVX512
    virtual void RenderFrame(glim::Camera& cam, glm::uvec2 viewSize);
    virtual void DrawSettings(glim::SettingStore& settings);
#else
    virtual void RenderFrame(glim::Camera& cam, glm::uvec2 viewSize) { }
    virtual void DrawSettings(glim::SettingStore& settings) { ImGui::Text("CPU renderer must be compiled with AVX512"); }
#endif

private:
    std::shared_ptr<VoxelMap> _map;
    std::unique_ptr<FlatVoxelStorage> _storage;

    std::shared_ptr<ogl::Shader> _blitShader;
    std::shared_ptr<ogl::Buffer> _pbo;
    std::unique_ptr<ogl::Texture2D> _accumTex;

    bool _enablePathTracer = false;
    uint32_t _frameNo = 0;

    glim::TimeStat _frameTime;
};