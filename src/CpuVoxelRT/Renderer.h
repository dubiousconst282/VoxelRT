#pragma once

#include <glm/glm.hpp>
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

struct GpuRenderer : public Renderer {
    std::shared_ptr<ogl::Shader> _mainShader;
    bool _showHeatmap;

    glim::TimeStat _frameTime;
    GLuint _frameQueryObj = 0;
    std::unique_ptr<ogl::Buffer> _metricsBuffer;

    std::shared_ptr<VoxelMap> _map;
    std::unique_ptr<GpuVoxelStorage> _storage;

public:
    GpuRenderer(ogl::ShaderLib& shlib, std::shared_ptr<VoxelMap> map);
    ~GpuRenderer();

    virtual void RenderFrame(glim::Camera& cam, glm::uvec2 viewSize);
    virtual void DrawSettings(glim::SettingStore& settings);
};
struct CpuRenderer : public Renderer {
    CpuRenderer(ogl::ShaderLib& shlib) {}
    virtual void RenderFrame(glim::Camera& cam, glm::uvec2 viewSize) {}
    virtual void DrawSettings(glim::SettingStore& settings) {}
};