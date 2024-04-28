#include <cstdint>
#include <execution>
#include <memory>
#include <ranges>

#include <SwRast/SIMD.h>
#include <SwRast/Texture.h>

#include "Renderer.h"

#include "Rendering/GBuffer.h"

// Cannot be > 2048*512*2048 because memory index is signed 32-bits
using ViewSectorIndexer =
    LinearIndexer3D<11 - MaskIndexer::ShiftXZ - BrickIndexer::ShiftXZ,
                    9 - MaskIndexer::ShiftY - BrickIndexer::ShiftY, false>;

using BrickMaskIndexer = LinearIndexer3D<BrickIndexer::ShiftXZ - 2, BrickIndexer::ShiftY - 2, false>;

struct FlatVoxelStorage {
    std::unique_ptr<uint8_t[]> StorageBuffer;
    std::unique_ptr<uint64_t[]> OccupancyStorage;
    uint64_t SectorMasks[ViewSectorIndexer::MaxArea] = {};
    uint64_t Palette[256];

    FlatVoxelStorage() {
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
        return {
            conv2f((MaterialData >> 11) & 31) * (1.0f / 31),
            conv2f((MaterialData >> 5) & 63) * (1.0f / 63),
            conv2f((MaterialData >> 0) & 31) * (1.0f / 31),
        };
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
static VInt GetVoxelMaterial(const FlatVoxelStorage& map, VInt3 pos, VMask mask) {
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
static VMask GetStepPos(const FlatVoxelStorage& map, VInt3& pos, VFloat3 dir, VMask mask) {
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
    // TODO: can this be optimized to 1x ternlog using shift to get dir sign? bypass latency probably cheaper than cmp
    pos.x.set_if(mask, csel(dir.x < 0, (pos.x & ~cellMask), (pos.x | cellMask)));
    pos.y.set_if(mask, csel(dir.y < 0, (pos.y & ~cellMask), (pos.y | cellMask)));
    pos.z.set_if(mask, csel(dir.z < 0, (pos.z & ~cellMask), (pos.z | cellMask)));

    return level0;
}

// TODO: consider 2x unroll to help hide gather latency from better ILP, need to profile first
static VHitResult RayCast(const FlatVoxelStorage& map, VFloat3 origin, VFloat3 dir, VMask activeMask, glm::ivec3 worldOrigin) {
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
    VFloat4 nearPos = TransformVector(invProjMat, { uv.x, uv.y, 0, 1 });
    VFloat4 farPos = nearPos + VFloat4(invProjMat[2]);
    rayPos = VFloat3(nearPos) * (1.0f / nearPos.w);
    rayDir = VFloat3(farPos) * (1.0f / farPos.w);

    rayDir = normalize(rayDir);
}

CpuRenderer::CpuRenderer(ogl::ShaderLib& shlib, std::shared_ptr<VoxelMap> map) {
    _map = std::move(map);
    _storage = std::make_unique<FlatVoxelStorage>();
    _gbuffer = std::make_unique<GBuffer>(shlib);

    _blitShader = shlib.LoadComp("CopyTiledFramebuffer");
    _map->MarkAllDirty();
}

CpuRenderer::~CpuRenderer() = default;

struct Framebuffer {
    struct alignas(64) Tile {
        VInt Albedo;        // RGBA8, A = normal
        VFloat Depth;
        VInt IrradianceRG;  // F16
        VInt IrradianceBX;  // F16, u16 unused
    };
    uint32_t Width, Height, TileStride;
    uint32_t TileShiftX, TileShiftY;
    Tile Tiles[];
};

void CpuRenderer::RenderFrame(glim::Camera& cam, glm::uvec2 viewSize) {
#ifndef NDEBUG  // debug builds are slow af
    viewSize /= 4;
#endif
    viewSize &= ~3u;  // round down to 4x4 steps

    _storage->SyncBuffers(*_map);

    uint32_t tilesX = viewSize.x / simd::TileWidth;
    uint32_t tilesY = viewSize.y / simd::TileHeight;
    size_t fbSize = (tilesX * tilesY * sizeof(Framebuffer::Tile)) + sizeof(Framebuffer);
    if (_pbo == nullptr || _pbo->Size < fbSize) {
        _pbo = std::make_unique<ogl::Buffer>(fbSize, GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT);
    }

    _gbuffer->SetCamera(cam, viewSize, false);

    glm::mat4 invProj = _gbuffer->CurrentInvProj;
    // Bias matrix to take UVs in range [0..screen] rather than [-1..1]
    invProj = glm::translate(invProj, glm::vec3(-1.0f, -1.0f, 0.0f));
    invProj = glm::scale(invProj, glm::vec3(2.0f / viewSize.x, 2.0f / viewSize.y, 1.0f));

    // Writing to memory mapping is ~1ms slower than local buffer at 1080p,
    // but temp buffer + BufferSubData() is even slower... though probably faster for dGPUs?
    // auto fb = (Framebuffer*)_fbData.get();
    auto fb = _pbo->Map<Framebuffer>(GL_MAP_WRITE_BIT);
    fb->Width = viewSize.x;
    fb->Height = viewSize.y;
    fb->TileStride = viewSize.x / simd::TileWidth;
    fb->TileShiftX = (uint32_t)std::countr_zero(simd::TileWidth);
    fb->TileShiftY = (uint32_t)std::countr_zero(simd::TileHeight);

    _frameTime.Begin();

    static swr::HdrTexture2D _skyBox = swr::texutil::LoadCubemapFromPanoramaHDR("assets/skyboxes/evening_road_01_puresky_4k.hdr");
    glm::ivec3 worldOrigin = glm::floor(cam.ViewPosition);

    auto rows = std::ranges::iota_view(0u, viewSize.y / simd::TileHeight);
    std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), [&](uint32_t rowId) {
        VRandom rng(rowId + _gbuffer->FrameNo * 123456ull);
        uint32_t y = rowId * simd::TileHeight;
        auto tile = &fb->Tiles[(y / simd::TileHeight) * fb->TileStride];

        for (uint32_t x = 0; x < viewSize.x; x += simd::TileWidth) {
            VFloat u = simd::conv2f((int32_t)x + simd::TileOffsetsX) + 0.5f; // + rng.NextUnsignedFloat() - 0.5f;
            VFloat v = simd::conv2f((int32_t)y + simd::TileOffsetsY) + 0.5f; // + rng.NextUnsignedFloat() - 0.5f;

            VFloat3 origin, dir;
            GetPrimaryRay({ u, v }, invProj, origin, dir);
            origin += VFloat3(glm::fract(_gbuffer->CurrentPos));

            VInt albedo;
            VFloat depth;
            VFloat3 irradiance = 0.0f;
            VFloat3 throughput = 1.0f;
            VMask mask = (VMask)(~0);

            for (uint32_t i = 0; i < 3 && any(mask); i++) {
                auto hit = RayCast(*_storage, origin, dir, mask, worldOrigin);

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

                    matColor.x.set_if(missMask, skyColor.x);
                    matColor.y.set_if(missMask, skyColor.y);
                    matColor.z.set_if(missMask, skyColor.z);
                    emissionStrength.set_if(missMask, 1.0f);
                }
                if (i == 0) {
                    VInt packedNorm = (round2i(hit.Normal.x) + 1) << 24 |
                                      (round2i(hit.Normal.y) + 1) << 26 |
                                      (round2i(hit.Normal.z) + 1) << 28;
                    albedo = swr::pixfmt::RGBA8u::Pack({ matColor, 0.0f }) | packedNorm;
                    depth = hit.Distance;

                    if (!_enablePathTracer) {
                        irradiance = 1.0f;
                        break;
                    }
                } else {
                    throughput *= matColor;
                }
                irradiance += throughput * emissionStrength;
                mask &= hit.Mask;

                origin = hit.Pos + hit.Normal * 0.01f;
                dir = simd::normalize(hit.Normal + rng.NextDirection());  // lambertian
            }
            // Write out entire tile at once in hopes for better write-combining or whatever
            *tile++ = {
                .Albedo = albedo,
                .Depth = depth,
                .IrradianceRG = swr::pixfmt::RG16f::Pack({ irradiance.x, irradiance.y }),
                .IrradianceBX = swr::pixfmt::RG16f::Pack({ irradiance.z }),
            };
        }
    });

    _frameTime.End();

    // Present
    _gbuffer->SetUniforms(*_blitShader);
    _blitShader->SetUniform("ssbo_FrameData", *_pbo);

    uint32_t groupsX = (viewSize.x + 7) / 8, groupsY = (viewSize.y + 7) / 8;
    _blitShader->DispatchCompute(groupsX, groupsY, 1);

    _gbuffer->DenoiseAndPresent();
}
void CpuRenderer::DrawSettings(glim::SettingStore& settings) {
    ImGui::SeparatorText("Renderer##CPU");
    settings.Checkbox("Path Trace", &_enablePathTracer);

    ImGui::Separator();
    _frameTime.Draw("Frame Time");

    if (_gbuffer->CurrentTex != nullptr) {
        double frameMs, frameDevMs;
        _frameTime.GetElapsedMs(frameMs, frameDevMs);

        uint32_t numPixels = _gbuffer->CurrentTex->Width * _gbuffer->CurrentTex->Height;
        uint32_t raysPerPixel = _enablePathTracer ? 3 : 1;
        double raysPerSec = numPixels * raysPerPixel * (1000 / frameMs);

        ImGui::Text("Rays/sec: %.2fM", raysPerSec / 1000000.0);
    }
}