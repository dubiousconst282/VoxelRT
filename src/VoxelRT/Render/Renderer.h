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
    DSVDF,          // 8-directional distance fields at 1:4 scale
    ESVO,           // 1:1 ESVO port
    // Tree64,      // 4³-tree
    Mesh,           // Rasterized greedy mesh
    Benchmark,
};

struct Renderer {
    Renderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : _ctx(ctx), _map(map) { }
    virtual ~Renderer() {}

    virtual void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) = 0;
    virtual void DrawSettings(glim::SettingStore& settings) { }

    static std::unique_ptr<Renderer> Create(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, RendererId id) {
        switch (id) {
            case RendererId::CPU: return Create<RendererId::CPU>(ctx, map);
            case RendererId::XBrickMap: return Create<RendererId::XBrickMap>(ctx, map);
            case RendererId::PlainDDA: return Create<RendererId::PlainDDA>(ctx, map);
            case RendererId::MultiDDA: return Create<RendererId::MultiDDA>(ctx, map);
            case RendererId::ManhattanDF: return Create<RendererId::ManhattanDF>(ctx, map);
            case RendererId::EuclideanDF: return Create<RendererId::EuclideanDF>(ctx, map);
            case RendererId::ESVO: return Create<RendererId::ESVO>(ctx, map);
            case RendererId::Mesh: return Create<RendererId::Mesh>(ctx, map);
            case RendererId::Benchmark: return Create<RendererId::Benchmark>(ctx, map);
            default: throw std::runtime_error("Unknown renderer ID");
        }
    }

    // Emit CPU -> GPU world synchronization commands.
    // *Must* be called at least once before RenderFrame().
    virtual bool SyncMap(glim::Camera& cam, havk::CommandList& cmds) { return false; }

protected:
    havk::DeviceContext* _ctx;
    std::shared_ptr<VoxelMap> _map;

private:
    template<RendererId>
    static std::unique_ptr<Renderer> Create(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);

    template<> std::unique_ptr<Renderer> Create<RendererId::CPU>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::XBrickMap>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::PlainDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::MultiDDA>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::ManhattanDF>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::EuclideanDF>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::ESVO>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::Mesh>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
    template<> std::unique_ptr<Renderer> Create<RendererId::Benchmark>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map);
};

struct FramePerfStats {
    enum Key {
        RayCasts,
        TraversalIters,
        ClocksPerRay,

        ReservedStart_ = 10,
        Frame_StartTS, Frame_EndTS,
        Sync_StartTS, Sync_EndTS,
        Count_,
    };
    static_assert(int(Key::Count_) <= 16); // keep in sync with PerfCounters.slang
    uint64_t Counters[16];
    uint32_t RayCastItersHistogram[32];
};

struct GpuRenderer : public Renderer {
    GpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map, RendererId type) : Renderer(ctx, map) {
        _renderShader = ctx->PipeBuilder->CreateCompute("Backends/Render.slang", { .PrepDefs = { { "BACKEND_ID", std::to_string((int)type) } } });

        _blueNoiseTex = havk::Image::LoadFile(ctx, "assets/bluenoise/stbn_vec2_2Dx1D_128x128x64_combined.png",
                                              VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_R8G8_UINT, 1);
        _skyboxTex = havk::Image::LoadFilePanoramaToCube(ctx, "assets/skyboxes/evening_road_01_puresky_4k.hdr");

        PerfCounterBuffer = ctx->CreateTransientBuffer({
            .Size = sizeof(FramePerfStats),
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        });
        TimeQueryPool = ctx->CreateQueryPool(VK_QUERY_TYPE_TIMESTAMP, 4);
    }

    void DrawPerfCounters();

    FramePerfStats LastFrameStats;
    havk::TransientBufferPtr PerfCounterBuffer;
    havk::QueryPoolPtr TimeQueryPool;

protected:
    havk::ImagePtr _blueNoiseTex;
    havk::ImagePtr _skyboxTex;
    havk::ComputePipelinePtr _renderShader;

    void DispatchRenderShader(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds, auto map) {
        TimeQueryPool->CopyResults(cmds, *PerfCounterBuffer->DeviceBuffer, offsetof(FramePerfStats, Counters[FramePerfStats::Frame_StartTS]));

        memcpy(&LastFrameStats, PerfCounterBuffer->ReadBack(cmds), sizeof(FramePerfStats));

        // Rescale timestamps
        for (uint32_t i = 0; i < TimeQueryPool->NumQueries; i++) {
            uint32_t j = FramePerfStats::Frame_StartTS + i;
            LastFrameStats.Counters[j] = TimeQueryPool->GetTimestampNanos(LastFrameStats.Counters[j]);
        }
        
        PerfCounterBuffer->Write(cmds, nullptr);  // reset counters

        struct RenderParams {
            decltype(map) Map;
            VkDeviceAddress GBuffer;  // GBufferUniforms*

            uint32_t MaxBounces;
            havk::ImageHandle StbnTexture;
            havk::ImageHandle SkyTexture;
            VkDeviceAddress PerfCounters;
        };

        TimeQueryPool->WriteTimestamp(cmds, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

        uint32_t groupsX = (target->RenderSize.x + 7) / 8, groupsY = (target->RenderSize.y + 7) / 8;

        cmds.Barrier(*target->AlbedoTex, havk::UseBarrier::ComputeReadWrite, VK_IMAGE_LAYOUT_GENERAL);
        _renderShader->Dispatch(cmds, { groupsX, groupsY, 1 }, RenderParams {
            .Map = map,
            .GBuffer = cmds.GetDeviceAddress(*target->UniformBuffer, havk::UseBarrier::ComputeRead),
            .MaxBounces = target->NumLightBounces,
            .StbnTexture = cmds.GetDescriptorHandle(*_blueNoiseTex, havk::UseBarrier::ComputeRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            .SkyTexture = cmds.GetDescriptorHandle(*_skyboxTex, havk::UseBarrier::ComputeRead, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            .PerfCounters = cmds.GetDeviceAddress(*PerfCounterBuffer->DeviceBuffer, havk::UseBarrier::ComputeReadWrite),
        });

        TimeQueryPool->WriteTimestamp(cmds, 1, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
};