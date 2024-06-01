#pragma once

#include <cstdint>
#include <memory>

#include <glm/glm.hpp>

#include <imgui.h>
#include <Havk/Havk.h>

#include <Common/Camera.h>
#include <Common/SettingStore.h>

#include "VoxelMap.h"
#include "GBuffer.h"

struct Renderer {
    virtual ~Renderer() {}

    virtual void RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds) = 0;
    virtual void DrawSettings(glim::SettingStore& settings) {}

protected:
    havk::DeviceContext* _ctx;
    std::shared_ptr<VoxelMap> _map;
    
    std::unique_ptr<GBuffer> _gbuffer;
    havk::GraphicsPipelinePtr _blitShader;

    uint32_t _numLightBounces = 1;
    glim::TimeStat _frameTime;
};

struct GpuVoxelStorage;
struct FlatVoxelStorage;

struct GpuRenderer : public Renderer {
    GpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    ~GpuRenderer();

    virtual void RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds);
    virtual void DrawSettings(glim::SettingStore& settings);

private:
    std::unique_ptr<GpuVoxelStorage> _storage;

    /* std::shared_ptr<ogl::Shader> _renderShader;
     std::unique_ptr<ogl::Texture2D> _blueNoiseTex;
     std::unique_ptr<ogl::TextureCube> _skyTex;
     std::unique_ptr<ogl::Buffer> _rayCellInteractionMaskLUT;
 
    bool _useAnisotropicLods;
*/
};
struct CpuRenderer : public Renderer {
    CpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    ~CpuRenderer();

    virtual void RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds);
    virtual void DrawSettings(glim::SettingStore& settings);

private:
    std::unique_ptr<FlatVoxelStorage> _storage;
    havk::ComputePipelinePtr _blitShader;
};