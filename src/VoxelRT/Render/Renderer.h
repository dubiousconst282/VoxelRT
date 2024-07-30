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
    XBrickMap,      // 3-level grid with 4³ sectors to 8³ bricks, plus 4³ masks for finer space skipping
    PlainDDA,       // Flat grid, unaccelerated DDA
    MultiDDA,       // Flat grid, space skipping through 8³ bricks using nested DDA loops
    ManhattanDF,    // Flat grid of 128³ tiled distance fields, sparse allocation
    EuclideanDF,
    // DirectionalDF,// 8-directional distance fields at 1:2 scale
    // ESVO         // 1:1 ESVO port
    // Tree64,      // 4³-tree
    // Tree512,     // 8³-tree
};

struct Renderer {
    virtual ~Renderer() {}

    virtual void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) = 0;
    virtual void DrawSettings(glim::SettingStore& settings) {
        settings.Slider("Light Bounces", &_numLightBounces, 1, 0u, 5u);
    }

    static std::unique_ptr<Renderer> Create(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, RendererId id) {
        switch (id) {
            case RendererId::CPU: return Create<RendererId::CPU>(ctx, map);
            case RendererId::XBrickMap: return Create<RendererId::XBrickMap>(ctx, map);
            case RendererId::PlainDDA: return Create<RendererId::PlainDDA>(ctx, map);
            case RendererId::MultiDDA: return Create<RendererId::MultiDDA>(ctx, map);
            case RendererId::ManhattanDF: return Create<RendererId::ManhattanDF>(ctx, map);
            case RendererId::EuclideanDF: return Create<RendererId::EuclideanDF>(ctx, map);
            default: throw std::runtime_error("Unknown renderer ID");
        }
    }

protected:
    havk::DeviceContext* _ctx;
    std::shared_ptr<VoxelMap> _map;

    uint32_t _numLightBounces = 1;

private:
    template<RendererId>
    static std::unique_ptr<Renderer> Create(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);

    template<> std::unique_ptr<Renderer> Create<RendererId::CPU>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::XBrickMap>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::PlainDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::MultiDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::ManhattanDF>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::EuclideanDF>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
};

struct PerfStats {
    enum Key {
        RayCasts,
        TraversalIters,
        ClocksPerRay,

        ReservedStart_ = 12,
        Frame_StartQTS,
        Frame_EndQTS
    };
    uint64_t Counters[16];
    uint32_t RayCastItersHistogram[32];
};

struct GpuRenderer : public Renderer {
    GpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, RendererId type) {
        _ctx = ctx;
        _map = std::move(map);

        _renderShader = ctx->PipeBuilder->CreateCompute("Backends/Render.slang", { .PrepDefs = { { "BACKEND_ID", std::to_string((int)type) } } });

        _blueNoiseTex = havk::Image::LoadFile(ctx, "assets/bluenoise/stbn_vec2_2Dx1D_128x128x64_combined.png",
                                              VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_R8G8_UINT, 1);
        _skyboxTex = havk::Image::LoadFilePanoramaToCube(ctx, "assets/skyboxes/evening_road_01_puresky_4k.hdr");

        _perfCounterBuffer = ctx->CreateTransientBuffer({
            .Size = sizeof(PerfStats),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        });
        _renderQueryTs = ctx->CreateQueryPool(VK_QUERY_TYPE_TIMESTAMP, 2);
    }

    void DrawPerfCounters();

protected:
    havk::ImagePtr _blueNoiseTex;
    havk::ImagePtr _skyboxTex;
    havk::ComputePipelinePtr _renderShader;

    havk::TransientBufferPtr _perfCounterBuffer;
    havk::QueryPoolPtr _renderQueryTs;
    PerfStats _stats;

    void DispatchRenderShader(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds, auto map) {
        _renderQueryTs->CopyResults(cmds, *_perfCounterBuffer->DeviceBuffer, offsetof(PerfStats, Counters[PerfStats::Frame_StartQTS]));

        memcpy(&_stats, _perfCounterBuffer->ReadBack(cmds), sizeof(PerfStats));
        _perfCounterBuffer->Write(cmds, nullptr);  // reset counters

        struct RenderParams {
            decltype(map) Map;
            VkDeviceAddress GBuffer;  // GBufferUniforms*

            uint32_t MaxBounces;
            havk::ImageHandle StbnTexture;
            havk::ImageHandle SkyTexture;
            VkDeviceAddress PerfCounters;
        };

        _renderQueryTs->WriteTimestamp(cmds, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

        uint32_t groupsX = (target->RenderSize.x + 7) / 8, groupsY = (target->RenderSize.y + 7) / 8;

        cmds.Barrier(*target->AlbedoTex, havk::UseBarrier::ComputeReadWrite, VK_IMAGE_LAYOUT_GENERAL);
        _renderShader->Dispatch(cmds, { groupsX, groupsY, 1 }, RenderParams {
            .Map = map,
            .GBuffer = cmds.GetDeviceAddress(*target->UniformBuffer, havk::UseBarrier::ComputeRead),
            .MaxBounces = _numLightBounces,
            .StbnTexture = cmds.GetDescriptorHandle(*_blueNoiseTex, havk::UseBarrier::ComputeRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            .SkyTexture = cmds.GetDescriptorHandle(*_skyboxTex, havk::UseBarrier::ComputeRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            .PerfCounters = cmds.GetDeviceAddress(*_perfCounterBuffer->DeviceBuffer, havk::UseBarrier::ComputeReadWrite),
        });

        _renderQueryTs->WriteTimestamp(cmds, 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
};