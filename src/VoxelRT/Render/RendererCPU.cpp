#include <cstdint>
#include <execution>
#include <memory>
#include <ranges>

#include <SwRast/SIMD.h>
#include <SwRast/Texture.h>

#include "Renderer.h"

// Cannot be > 2048*512*2048 because memory index is signed 32-bits
using ViewSectorIndexer =
    LinearIndexer3D<11 - MaskIndexer::ShiftXZ - BrickIndexer::ShiftXZ,
                    9 - MaskIndexer::ShiftY - BrickIndexer::ShiftY, false>;

using BrickMaskIndexer = LinearIndexer3D<BrickIndexer::ShiftXZ - 2, BrickIndexer::ShiftY - 2, false>;

struct StorageManager {
    std::unique_ptr<uint8_t[]> StorageBuffer;
    std::unique_ptr<uint64_t[]> OccupancyStorage;
    uint64_t SectorMasks[ViewSectorIndexer::MaxArea] = {};
    uint64_t Palette[256];

    StorageManager() {
        size_t storageCap = ViewSectorIndexer::MaxArea * (MaskIndexer::MaxArea * BrickIndexer::MaxArea);
        // TODO: implement sparse memory alloc using VirtualAlloc? page remapping could also be useful for something
        StorageBuffer = std::make_unique<uint8_t[]>(storageCap);
        OccupancyStorage = std::make_unique<uint64_t[]>(storageCap / 64);
    }

    void SyncBuffers(VoxelMap& map) {
        for (uint32_t i = 0; i < 256; i++) {
            Palette[i] = map.Palette[i].GetEncoded();
        }

        for (auto [sectorIdx, dirtyMask] : map.DirtyLocs) {
            glm::ivec3 sectorPos = WorldSectorIndexer::GetPos(sectorIdx);
            if (!ViewSectorIndexer::CheckInBounds(sectorPos)) continue;

            uint32_t sectorViewIdx = ViewSectorIndexer::GetIndex(sectorPos);
            if (!map.Sectors.contains(sectorIdx)) {
                SectorMasks[sectorViewIdx] = 0;
                continue;
            }

            Sector& sector = map.Sectors[sectorIdx];
            uint64_t allocMask = sector.GetAllocationMask();
            SectorMasks[sectorViewIdx] = allocMask;

            for (uint32_t brickIdx : BitIter(dirtyMask & allocMask)) {
                uint32_t storageOffset = sectorViewIdx * (sizeof(Brick) * 64) + brickIdx * sizeof(Brick);

                Brick* brick = sector.GetBrick(brickIdx);
                std::memcpy(&StorageBuffer[storageOffset], brick, sizeof(Brick));
                UpdateOccupancy(brick, storageOffset / 64);
            }
        }
        map.DirtyLocs.clear();
    }

    void UpdateOccupancy(Brick* brick, uint32_t storageOffset) {
        const uint32_t BrickSize = BrickIndexer::SizeXZ;

        uint64_t* cells = &OccupancyStorage[storageOffset];

        // clang-format off
        for (uint32_t cy = 0; cy < BrickSize; cy += 4)
        for (uint32_t cz = 0; cz < BrickSize; cz += 4)
        for (uint32_t cx = 0; cx < BrickSize; cx += 4) {
            uint64_t mask = 0;

            for (uint32_t vy = 0; vy < 4; vy++)
            for (uint32_t vz = 0; vz < 4; vz++)
            for (uint32_t vx = 0; vx < 4; vx++) {
                bool occupied = !brick->Data[BrickIndexer::GetIndex(cx + vx, cy + vy, cz + vz)].IsEmpty();
                mask |= uint64_t(occupied) << (vx + vz * 4 + vy * 16);
            }
            cells[BrickMaskIndexer::GetIndex(glm::uvec3(cx, cy, cz) / 4u)] = mask;
        }
        // clang-format on
    }
};

using namespace simd;

struct VHitResult {
    VInt MaterialData;
    VFloat Distance;
    VFloat3 Pos;
    VFloat3 Normal;
    VFloat2 UV;

    VMask Mask;

    VFloat3 GetColor() const {
        VFloat3 color = {
            conv2f((MaterialData >> 11) & 31) * (1.0f / 31),
            conv2f((MaterialData >> 5) & 63) * (1.0f / 63),
            conv2f((MaterialData >> 0) & 31) * (1.0f / 31),
        };
        return color * color;
    }
    VFloat GetEmissionStrength() const {
        return swr::pixfmt::RG16f::Unpack(MaterialData).y;
    }
};

static const uint32_t SectorVoxelShiftXZ = MaskIndexer::ShiftXZ + BrickIndexer::ShiftXZ;
static const uint32_t SectorVoxelShiftY = MaskIndexer::ShiftY + BrickIndexer::ShiftY;

// Creates mask for voxel coords that are inside the brick map.
static VMask GetInboundMask(VInt x, VInt y, VInt z) {
    return simd::ucmp_lt(x | z, ViewSectorIndexer::SizeXZ << SectorVoxelShiftXZ) &
           simd::ucmp_lt(y, ViewSectorIndexer::SizeY << SectorVoxelShiftY);
}

// 2 dependent gathers: >=50 latency + index calc
static VInt GetVoxelMaterial(const StorageManager& map, VInt3 pos, VMask mask) {
    VInt sectorIdx = ViewSectorIndexer::GetIndex(pos.x >> SectorVoxelShiftXZ, pos.y >> SectorVoxelShiftY, pos.z >> SectorVoxelShiftXZ);
    VInt maskIdx = MaskIndexer::GetIndex(pos.x >> BrickIndexer::ShiftXZ, pos.y >> BrickIndexer::ShiftY, pos.z >> BrickIndexer::ShiftXZ);
    VInt voxelIdx = BrickIndexer::GetIndex(pos.x, pos.y, pos.z);

    VInt slotIdx = sectorIdx * (sizeof(Brick) * 64) + maskIdx * sizeof(Brick) + voxelIdx;

    // Do 4-aligned gather to avoid crossing cache/pages
    VInt voxelIds = VInt::mask_gather<4>(map.StorageBuffer.get(), slotIdx >> 2, mask);
    voxelIds = voxelIds >> ((slotIdx & 3) * 8) & 255;

    return VInt::mask_gather<8>(map.Palette, voxelIds, mask);
}

// 2/4 independet gathers: >=30/60 latency + ALU
static VMask GetStepPos(const StorageManager& map, VInt3& pos, VFloat3 dir, VMask mask) {
    VInt sectorIdx = ViewSectorIndexer::GetIndex(pos.x >> SectorVoxelShiftXZ, pos.y >> SectorVoxelShiftY, pos.z >> SectorVoxelShiftXZ);

    VInt mask_0 = VInt::mask_gather<8>((uint8_t*)map.SectorMasks + 0, sectorIdx, mask);
    VInt mask_32 = VInt::mask_gather<8>((uint8_t*)map.SectorMasks + 4, sectorIdx, mask);

    VInt maskIdx = MaskIndexer::GetIndex(pos.x >> BrickIndexer::ShiftXZ, pos.y >> BrickIndexer::ShiftY, pos.z >> BrickIndexer::ShiftXZ);
    VInt currMask = csel(maskIdx < 32, mask_0, mask_32);
    VMask level0 = (currMask >> (maskIdx & 31) & 1) != 0;
    VInt lod = 3;

    if (simd::any(level0)) {
        VInt cellIdx = (sectorIdx * (sizeof(Brick) * 64) + maskIdx * sizeof(Brick)) >> 6;
        cellIdx += BrickMaskIndexer::GetIndex(pos.x >> 2, pos.y >> 2, pos.z >> 2);

        maskIdx.set_if(level0, MaskIndexer::GetIndex(pos.x, pos.y, pos.z));
        lod.set_if(level0, 0);

        mask_0.set_if(level0, VInt::mask_gather<8>((uint8_t*)map.OccupancyStorage.get() + 0, cellIdx, mask & level0));
        mask_32.set_if(level0, VInt::mask_gather<8>((uint8_t*)map.OccupancyStorage.get() + 4, cellIdx, mask & level0));

        currMask = csel(maskIdx < 32, mask_0, mask_32);
        level0 = (currMask >> (maskIdx & 31) & 1) != 0;
    }

    VMask level4 = (mask_0 | mask_32) == 0;
    VMask level2 = (currMask >> (maskIdx & 0xA) & 0x00330033) == 0;
    lod += csel(level4, 2, csel(level2, 1, VInt(0)));

    VInt cellMask = (1 << lod) - 1;
    // this could be optimized into srai+ternlog but will hardly matter amidst this sea of gathers.
    pos.x.set_if(mask, csel(dir.x < 0, (pos.x & ~cellMask), (pos.x | cellMask)));
    pos.y.set_if(mask, csel(dir.y < 0, (pos.y & ~cellMask), (pos.y | cellMask)));
    pos.z.set_if(mask, csel(dir.z < 0, (pos.z & ~cellMask), (pos.z | cellMask)));

    return level0;
}
static VHitResult RayCast(const StorageManager& map, VFloat3 origin, VFloat3 dir, VMask activeMask, glm::ivec3 worldOrigin) {
    VFloat3 invDir = 1.0f / dir;
    // VFloat3 tStart = (max(sign(dir), 0.0) - origin) * invDir;
    VFloat3 tStart = {
        (csel(dir.x < 0, VFloat(0.0), 1.0f) - origin.x) * invDir.x,
        (csel(dir.y < 0, VFloat(0.0), 1.0f) - origin.y) * invDir.y,
        (csel(dir.z < 0, VFloat(0.0), 1.0f) - origin.z) * invDir.z,
    };
    VFloat3 sideDist = 0.0f;
    VFloat3 currPos = origin;
    VInt3 voxelPos;
    VMask inboundMask = 0;

    for (uint32_t i = 0; i < 128; i++) {
        voxelPos = worldOrigin + VInt3(floor2i(currPos.x), floor2i(currPos.y), floor2i(currPos.z));

        inboundMask = GetInboundMask(voxelPos.x, voxelPos.y, voxelPos.z);
        activeMask &= inboundMask;
        VMask hitMask = GetStepPos(map, voxelPos, dir, activeMask);

        activeMask &= ~hitMask;
        if (!any(activeMask)) break;

        voxelPos -= worldOrigin;
        sideDist.x.set_if(activeMask, tStart.x + conv2f(voxelPos.x) * invDir.x);
        sideDist.y.set_if(activeMask, tStart.y + conv2f(voxelPos.y) * invDir.y);
        sideDist.z.set_if(activeMask, tStart.z + conv2f(voxelPos.z) * invDir.z);

        VFloat tmin = min(min(sideDist.x, sideDist.y), sideDist.z) + 0.001f;
        currPos = origin + tmin * dir;
    }

    VFloat hitDist = min(min(sideDist.x, sideDist.y), sideDist.z);
    VMask sideMaskX = sideDist.x == hitDist;
    VMask sideMaskY = sideDist.y == hitDist;
    VMask sideMaskZ = ~sideMaskX & ~sideMaskY;

    return {
        .MaterialData = GetVoxelMaterial(map, voxelPos, ~activeMask),
        .Distance = hitDist,
        .Pos = currPos,
        .Normal = {
            csel(sideMaskX, (dir.x & -0.0f) ^ VFloat(-1.0f), 0),  // dir.x < 0 ? +1 : -1
            csel(sideMaskY, (dir.y & -0.0f) ^ VFloat(-1.0f), 0),
            csel(sideMaskZ, (dir.z & -0.0f) ^ VFloat(-1.0f), 0),
        },
        .UV = {
            fract(csel(sideMaskX, currPos.y, currPos.x)),
            fract(csel(sideMaskZ, currPos.y, currPos.z)),
        },
        .Mask = (VMask)(~activeMask & inboundMask),
    };
}

static void GetPrimaryRay(VFloat2 uv, const glm::mat4& invProjMat, VFloat3& rayPos, VFloat3& rayDir) {
    VFloat4 farPos = TransformVector(invProjMat, { uv.x, uv.y, 1, 1 });
    rayPos = VFloat3(0.0);
    rayDir = normalize(VFloat3(farPos) * (1.0f / farPos.w));
}

struct alignas(64) VBlueNoise {
    uint16_t Data[128 * 128 * 64];  // Tiled to simd size

    VBlueNoise() {
        auto image = swr::StbImage::Load("assets/bluenoise/stbn_vec2_2Dx1D_128x128x64_combined.png");
        uint16_t* ptr = Data;

        for (uint32_t y = 0; y < 128 * 64; y += simd::TileHeight) {
            for (uint32_t x = 0; x < 128; x += simd::TileWidth) {
                for (uint32_t i = 0; i < simd::VectorWidth; i++) {
                    uint32_t sx = x + (i % simd::TileWidth);
                    uint32_t sy = y + (i / simd::TileWidth);
                    uint8_t* s = &image.Data[(sx + sy * 128) * 4];
                    *ptr++ = s[0] | s[1] << 8;
                }
            }
        }
    }

    VFloat2 Sample(glm::uvec2 pos, uint32_t frameIdx, uint32_t sampleIdx) const {
        assert(pos.x % simd::TileWidth == 0);
        assert(pos.y % simd::TileHeight == 0);

        glm::vec2 sampleOffset = glm::fract(float(sampleIdx) * glm::vec2(0.75487766624669276005f, 0.56984029099805326591f) + 0.5f);
        pos = (pos + glm::uvec2(sampleOffset * 128.0f)) & 127u;
        pos.y += (frameIdx & 63) * 128;

        uint32_t offset = (pos.x / simd::TileWidth) + (pos.y / simd::TileHeight) * (128 / simd::TileWidth);
        offset = offset * simd::VectorWidth;
#if SIMD_AVX512
        VInt values = _mm512_cvtepu16_epi32(_mm256_load_epi32(&Data[offset]));
#else  // SIMD_AVX2
        VInt values = _mm256_cvtepu16_epi32(_mm_load_si128((__m128i*)&Data[offset]));
#endif
        return VFloat2(conv2f(values & 255), conv2f(values >> 8)) * (1.0 / 255);
    }
};

static VFloat3 SampleDirection(VFloat2 sample) {
    // azimuth = rand() * PI * 2
    // y = rand() * 2 - 1
    // sin_elevation = sqrt(1 - y * y)
    // x = sin_elevation * cos(azimuth);
    // z = sin_elevation * sin(azimuth);
    // 
    // VInt rand = NextU32();
    // const float randScale = (1.0f / (1 << 15));
    // VFloat y = simd::conv2f(rand >> 16) * randScale;  // signed
    // VFloat a = simd::conv2f(rand & 0x7FFF) * randScale;
    VFloat y = sample.y * 2.0f - 1.0f;

    VFloat x, z;
    simd::sincos_2pi(sample.x, x, z);
    VFloat sy = simd::approx_sqrt(1.0f - y * y);

    return { x * sy, y, z * sy };
}

static VFloat3 SampleHemisphere(VFloat2 sample, const VFloat3& normal) {
    VFloat3 dir = SampleDirection(sample);
    VFloat sign = simd::dot(dir, normal) & -0.0f;
    return { dir.x ^ sign, dir.y ^ sign, dir.z ^ sign };
}

struct RenderedTile {
    VInt Albedo;        // RGBA8, A = normal
    VFloat Depth;
    VInt IrradianceRG;  // F16
    VInt IrradianceBX;  // F16, u16 unused
};
static swr::HdrTexture2D _skyBox = swr::texutil::LoadCubemapFromPanoramaHDR("assets/skyboxes/evening_road_01_puresky_4k.hdr");
static VBlueNoise _blueNoise = VBlueNoise();

struct FrameConstants {
    StorageManager& Storage;
    glm::uvec2 Size;
    glm::ivec3 WorldOrigin;
    glm::vec3 OriginFrac;
    uint32_t FrameNo;
    uint32_t NumLightBounces;
    glm::mat4 CurrentProj;
    glm::mat4 InvProj;
};

[[gnu::noinline]] // lambdas can't be debugged on release for some reason
static void RenderRow(const FrameConstants& fc, RenderedTile* dest, uint32_t y) {
    VFloat v = simd::conv2f((int32_t)y + simd::TileOffsetsY) + 0.5f;  // + rng.NextUnsignedFloat() - 0.5f;
    
    for (uint32_t x = 0; x < fc.Size.x; x += simd::TileWidth) {
        VFloat u = simd::conv2f((int32_t)x + simd::TileOffsetsX) + 0.5f;  // + rng.NextUnsignedFloat() - 0.5f;

        VFloat3 origin, dir;
        GetPrimaryRay({ u, v }, fc.InvProj, origin, dir);
        origin += VFloat3(fc.OriginFrac);

        VInt albedo;
        VFloat depth;
        VFloat3 irradiance = 0.0f;
        VFloat3 throughput = 1.0f;
        VMask mask = (VMask)(~0);

        for (uint32_t i = 0; i <= fc.NumLightBounces && any(mask); i++) {
            auto hit = RayCast(fc.Storage, origin, dir, mask, fc.WorldOrigin);

            VFloat3 matColor = hit.GetColor();
            VFloat emissionStrength = hit.GetEmissionStrength();

            VMask missMask = mask & ~hit.Mask;
            if (any(missMask)) {
                constexpr swr::SamplerDesc SD = {
                    .MagFilter = swr::FilterMode::Nearest,
                    .MinFilter = swr::FilterMode::Nearest,
                    .EnableMips = true,
                };
                VFloat3 skyColor = _skyBox.SampleCube<SD, false>(dir, i == 0 ? 1 : 3);
                skyColor *= 3;

                if (i == 0) {
                    // Special primary ray to prevent banding
                    irradiance.x.set_if(missMask, skyColor.x);
                    irradiance.y.set_if(missMask, skyColor.y);
                    irradiance.z.set_if(missMask, skyColor.z);
                } else {
                    matColor.x.set_if(missMask, skyColor.x);
                    matColor.y.set_if(missMask, skyColor.y);
                    matColor.z.set_if(missMask, skyColor.z);
                    emissionStrength.set_if(missMask, 1.0f);
                }
            }
            if (i == 0) {
                albedo = swr::pixfmt::RGBA8u::Pack({ matColor, 0.0f });
                albedo |= (round2i(hit.Normal.x) + 1) << 24;
                albedo |= (round2i(hit.Normal.y) + 1) << 26;
                albedo |= (round2i(hit.Normal.z) + 1) << 28;

                VFloat4 projPos = simd::TransformVector(fc.CurrentProj, { hit.Pos / 16.0f, 1.0f });  // scale down by 1/16 to minimize precision loss
                depth = csel(missMask, -1.0f, projPos.z / projPos.w);

                if (fc.NumLightBounces == 0) {
                    irradiance = 1.0f;
                    break;
                }
            } else {
                throughput *= matColor;
            }
            irradiance += throughput * emissionStrength;
            mask &= hit.Mask;

            origin = hit.Pos + hit.Normal * 0.01f;

            VFloat2 bn = _blueNoise.Sample(glm::uvec2(x, y), fc.FrameNo, i);
            dir = simd::normalize(hit.Normal + SampleDirection(bn));  // lambertian
        }
        // Write out entire tile at once in hopes for better write-combining or whatever
        *dest++ = {
            .Albedo = albedo,
            .Depth = depth,
            .IrradianceRG = swr::pixfmt::RG16f::Pack({ irradiance.x, irradiance.y }),
            .IrradianceBX = swr::pixfmt::RG16f::Pack({ irradiance.z }),
        };
    }
}

struct RendererCPU : public Renderer {
    RendererCPU(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : Renderer(ctx, map) {
        _storage = std::make_unique<StorageManager>();

        _blitShader = ctx->PipeBuilder->CreateCompute("Backends/ResolveCpuFramebuffer.slang");
        _map->MarkAllDirty();
    }

    void RenderFrame(glim::Camera& cam, GBuffer* target, havk::CommandList& cmds) override {
        glm::uvec2 renderSize = target->RenderSize;

        #if !NDEBUG
        renderSize /= 4; // debug builds are unusable without this
        #endif

        _storage->SyncBuffers(*_map);

        uint32_t tilesX = renderSize.x / simd::TileWidth;
        uint32_t tilesY = renderSize.y / simd::TileHeight;
        size_t fbSize = tilesX * tilesY * sizeof(RenderedTile);

        // Buffer orphaning to avoid having to deal with sync issues
        auto outputBuffer = _ctx->CreateBuffer({
            .Size = fbSize,
            .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
            .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        });

        auto destTiles = (RenderedTile*)outputBuffer->MappedData;

        FrameConstants fc = {
            .Storage = *_storage,
            .Size = renderSize,
            .WorldOrigin = glm::floor(target->CurrentPos),
            .OriginFrac = glm::fract(target->CurrentPos),
            .FrameNo = target->FrameNo,
            .NumLightBounces = target->NumLightBounces,
            .CurrentProj = target->CurrentProj,
            .InvProj = GBuffer::GetInverseProjScreenMat(target->CurrentProj, renderSize),
        };

        auto rows = std::ranges::iota_view(0u, renderSize.y / simd::TileHeight);
        std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), [&](uint32_t rowId) {
            // VRandom rng(rowId + _gbuffer->FrameNo * 123456ull);
            uint32_t y = rowId * simd::TileHeight;
            auto destRow = &destTiles[(y / simd::TileHeight) * (renderSize.x / simd::TileWidth)];

            RenderRow(fc, destRow, y);
        });
        outputBuffer->Flush();

        struct BlitParams {
            VkDeviceAddress GBuffer;
            VkDeviceAddress PixelData;

            uint32_t Width, Height, Stride;
            uint32_t TileShiftX, TileShiftY;
        };
        uint32_t groupsX = (renderSize.x + 7) / 8, groupsY = (renderSize.y + 7) / 8;
        
        cmds.Barrier(*target->AlbedoTex, havk::UseBarrier::ComputeReadWrite, VK_IMAGE_LAYOUT_GENERAL);
        _blitShader->Dispatch(cmds, { groupsX, groupsY, 1 }, BlitParams {
            .GBuffer = cmds.GetDeviceAddress(*target->UniformBuffer, havk::UseBarrier::ComputeRead),
            .PixelData = cmds.GetDeviceAddress(*outputBuffer, havk::UseBarrier::ComputeRead),
            .Width = renderSize.x,
            .Height = renderSize.y,
            .Stride = renderSize.x / simd::TileWidth,
            .TileShiftX = (uint32_t)std::countr_zero(simd::TileWidth),
            .TileShiftY = (uint32_t)std::countr_zero(simd::TileHeight),
        });
    }
    
private:
    std::unique_ptr<StorageManager> _storage;
    havk::ComputePipelinePtr _blitShader;
};

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::CPU>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererCPU>(ctx, map);
}