#pragma once

#include <cstdint>
#include <memory>

#include <Havk/Havk.h>

#include <glm/glm.hpp>
#include <Common/Camera.h>
#include <Common/SettingStore.h>

#include "../VoxelMap.h"
#include "GBuffer.h"

enum class RendererId {
    CPU,            // Minimal CPU-SIMD port of XBrickMap
    XBrickMap,      // 3-level grid with 4x4x4 sectors to 8x8x8 bricks, plus 4x4x4 masks for finer space skipping
    PlainDDA,       // Flat grid, unaccelerated DDA
    MultiDDA,       // Flat grid, space skipping through 8x8x8 bricks using nested DDA loops
    // ManhattanDF, // Flat grid, space skipping through Manhattan distance field
    // EuclideanDF, // Flat grid, space skipping through Euclidean distance field
    // DirectionalDF,// XBrickMap with (64/2)³ 8-directional distance fields
    // ESVO         // 1:1 ESVO port
    // Tree64,      // 4³-tree
    // Tree512,     // 8³-tree
};

struct Renderer {
    virtual ~Renderer() {}

    virtual void RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds) = 0;
    virtual void DrawSettings(glim::SettingStore& settings) {
        ImGui::SeparatorText("Renderer");
        ImGui::PushID(typeid(*this).hash_code());

        ImGui::PushItemWidth(150);
        settings.Combo("Debug Channel", &_gbuffer->DebugChannelView);
        settings.Slider("Light Bounces", &_numLightBounces, 1, 0u, 5u);
        settings.Slider("Denoiser Passes", &_gbuffer->NumDenoiserPasses, 1, 0u, 5u);

        char label[32];
        sprintf(label, "%.3gx (%dx%d)", _renderScale, _gbuffer->RenderSize.x, _gbuffer->RenderSize.y);
        settings.Slider("Render Scale", &_renderScale, 1, 0.25f, 4.0f, label);
        _renderScale = std::round(_renderScale / 0.25f) * 0.25f;

        settings.Checkbox("Temporal AA", &_gbuffer->EnableTAA);

        ImGui::PopItemWidth();
        ImGui::PopID();

        ImGui::Separator();
        _frameTime.Draw("Frame Time");

        auto totalIters = 1;
        if (_gbuffer->AlbedoTex != nullptr) {
            double frameMs, frameDevMs;
            _frameTime.GetElapsedMs(frameMs, frameDevMs);

            uint32_t numPixels = _gbuffer->RenderSize.x * _gbuffer->RenderSize.y;
            uint32_t raysPerPixel = (_numLightBounces + 1) * 2;  // sun
            double raysPerSec = numPixels * raysPerPixel * (1000 / frameMs);

            ImGui::Text("Rays/sec: %.2fM | Steps: %.3fM", raysPerSec / 1000000.0, totalIters / 1000000.0);
        }

        uint32_t numBricks = 0;
        for (auto& sector : _map->Sectors) {
            numBricks += (uint32_t)std::popcount(sector.second.GetAllocationMask());
        }
        ImGui::Text("Bricks: %.1fK in %.1fK sectors", numBricks / 1000.0, _map->Sectors.size() / 1000.0);
    }

    static std::unique_ptr<Renderer> Create(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, RendererId id) {
        switch (id) {
            case RendererId::CPU: return Create<RendererId::CPU>(ctx, map);
            case RendererId::XBrickMap: return Create<RendererId::XBrickMap>(ctx, map);
            case RendererId::PlainDDA: return Create<RendererId::PlainDDA>(ctx, map);
            case RendererId::MultiDDA: return Create<RendererId::MultiDDA>(ctx, map);
            default: throw std::runtime_error("Unknown renderer ID");
        }
    }

protected:
    havk::DeviceContext* _ctx;
    std::shared_ptr<VoxelMap> _map;
    
    std::unique_ptr<GBuffer> _gbuffer;
    havk::GraphicsPipelinePtr _blitShader;
    float _renderScale = 1.0f;

    uint32_t _numLightBounces = 1;
    glim::TimeStat _frameTime;

private:
    template<RendererId>
    static std::unique_ptr<Renderer> Create(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);

    template<> std::unique_ptr<Renderer> Create<RendererId::CPU>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::XBrickMap>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::PlainDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::MultiDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
};

struct GpuRenderer : public Renderer {
    GpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
        _ctx = ctx;
        _map = std::move(map);

        _gbuffer = std::make_unique<GBuffer>(ctx);
        _gbuffer->NumDenoiserPasses = 0;

        _blueNoiseTex = havk::Image::LoadFile(ctx, "assets/bluenoise/stbn_vec2_2Dx1D_128x128x64_combined.png",
                                              VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_R8G8_UINT, 1);
        _skyboxTex = havk::Image::LoadFilePanoramaToCube(ctx, "assets/skyboxes/evening_road_01_puresky_4k.hdr");
    }

protected:
    havk::ImagePtr _blueNoiseTex;
    havk::ImagePtr _skyboxTex;
};