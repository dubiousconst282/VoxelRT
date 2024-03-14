#include <cstdint>
#include <execution>
#include <memory>
#include <ranges>

#include <SwRast/SIMD.h>

#include "../Renderer.h"

#if SIMD_AVX512

// 1024³ bricks
// Cannot be > 2048*512*2048 because memory index is signed 32-bits
using ViewSectorIndexer =
    LinearIndexer3D<11 - BrickIndexer::ShiftXZ - VoxelIndexer::ShiftXZ,
                    9 - BrickIndexer::ShiftY - VoxelIndexer::ShiftY, false>;

using Level0Indexer = LinearIndexer3D<VoxelIndexer::ShiftXZ - 2, VoxelIndexer::ShiftY - 2, false>;

struct FlatVoxelStorage {
    std::unique_ptr<uint8_t[]> StorageBuffer;
    std::unique_ptr<uint64_t[]> OccupancyStorage;
    uint64_t SectorMasks[ViewSectorIndexer::MaxArea] = {};
    Material Palette[256];

    FlatVoxelStorage() {
        size_t storageCap = ViewSectorIndexer::MaxArea * (BrickIndexer::MaxArea * VoxelIndexer::MaxArea);
        // TODO: implement sparse memory alloc using VirtualAlloc? page remapping could also be useful for something
        StorageBuffer = std::make_unique<uint8_t[]>(storageCap);
        OccupancyStorage = std::make_unique<uint64_t[]>(storageCap / 64);
    }

    void SyncBuffers(VoxelMap& map) {
        auto itr = map.DirtyLocs.begin();

        // Allocate slots for dirty bricks
        while (itr != map.DirtyLocs.end()) {
            auto [sectorIdx, dirtyMask] = *itr;
            auto& sector = map.Sectors[sectorIdx];

            uint64_t emptyMask = sector.DeleteEmptyBricks();
            uint64_t allocMask = sector.GetAllocationMask();

            glm::ivec3 sectorPos = SectorIndexer::GetPos(sectorIdx);

            if (ViewSectorIndexer::CheckInBounds(sectorPos)) {
                uint32_t sectorViewIdx = ViewSectorIndexer::GetIndex(sectorPos);

                for (uint32_t brickIdx : BitIter(allocMask)) {
                    uint32_t storageOffset = sectorViewIdx * (sizeof(Brick) * 64) + brickIdx * sizeof(Brick);
                    // StorageBuffer.AllocRange(storageOffset, sizeof(Brick));
                    // OccupancyStorage.AllocRange(storageOffset / 64, sizeof(Brick) / 64);

                    Brick* brick = sector.GetBrick(brickIdx);
                    std::memcpy(&StorageBuffer[storageOffset], brick, sizeof(Brick));
                    UpdateOccupancy(brick, storageOffset / 64);
                }
                SectorMasks[sectorViewIdx] = allocMask;
            }

            if (allocMask == 0) {
                map.Sectors.erase(sectorIdx);
            }
            map.DirtyLocs.erase(itr++);
        }
        std::memcpy(Palette, map.Palette, sizeof(Palette));
    }

    void UpdateOccupancy(Brick* brick, uint32_t storageOffset) {
        const uint32_t BrickSize = VoxelIndexer::SizeXZ;

        uint64_t* cells = &OccupancyStorage[storageOffset];

        // clang-format off
        for (uint32_t cy = 0; cy < BrickSize; cy += 4)
        for (uint32_t cz = 0; cz < BrickSize; cz += 4)
        for (uint32_t cx = 0; cx < BrickSize; cx += 4) {
            uint64_t mask = 0;

            for (uint32_t vy = 0; vy < 4; vy++)
            for (uint32_t vz = 0; vz < 4; vz++)
            for (uint32_t vx = 0; vx < 4; vx++) {
                bool occupied = !brick->Data[VoxelIndexer::GetIndex(cx + vx, cy + vy, cz + vz)].IsEmpty();
                mask |= uint64_t(occupied) << (vx + vz * 4 + vy * 16);
            }
            cells[Level0Indexer::GetIndex(glm::uvec3(cx, cy, cz) / 4u)] = mask;
        }
    }
};

using namespace simd;

struct HitInfo {
    VInt MaterialData;
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
    VFloat GetEmissionStrength() const { return conv2f((MaterialData >> 16) & 15) * (7.0f / 15); }
};

static const uint32_t SectorVoxelShiftXZ = BrickIndexer::ShiftXZ + VoxelIndexer::ShiftXZ;
static const uint32_t SectorVoxelShiftY = BrickIndexer::ShiftY + VoxelIndexer::ShiftY;

// Creates mask for voxel coords that are inside the brick map.
static VMask GetInboundMask(VInt x, VInt y, VInt z) {
    return _mm512_cmplt_epu32_mask(x | z, _mm512_set1_epi32(ViewSectorIndexer::SizeXZ << SectorVoxelShiftXZ)) &
           _mm512_cmplt_epu32_mask(y, _mm512_set1_epi32(ViewSectorIndexer::SizeY << SectorVoxelShiftY));
}

// 2 dependent gathers: >=50 latency + index calc
static VInt GetVoxelMaterial(const FlatVoxelStorage& map, VInt3 pos, VMask mask) {
    VInt sectorIdx = ViewSectorIndexer::GetIndex(pos.x >> SectorVoxelShiftXZ, pos.y >> SectorVoxelShiftY, pos.z >> SectorVoxelShiftXZ);
    VInt maskIdx = BrickIndexer::GetIndex(pos.x >> VoxelIndexer::ShiftXZ, pos.y >> VoxelIndexer::ShiftY, pos.z >> VoxelIndexer::ShiftXZ);
    VInt voxelIdx = VoxelIndexer::GetIndex(pos.x,pos.y,pos.z);

    VInt slotIdx = sectorIdx*(sizeof(Brick)*64) + maskIdx*sizeof(Brick) + voxelIdx;

    // Do 4-aligned gather to avoid crossing cache/pages
    VInt voxelIds = VInt::mask_gather<4>(map.StorageBuffer.get(), slotIdx >> 2, mask);
    voxelIds = voxelIds >> ((slotIdx & 3) * 8) & 255;

    return VInt::mask_gather<4>(map.Palette, voxelIds, mask);
}

// 2/4 independet gathers: >=30/60 latency + ALU
static VMask GetStepPos(const FlatVoxelStorage& map, VInt3& pos, VFloat3 dir, VMask mask) {
    VInt sectorIdx = ViewSectorIndexer::GetIndex(pos.x >> SectorVoxelShiftXZ, pos.y >> SectorVoxelShiftY, pos.z >> SectorVoxelShiftXZ);

    VInt mask_0 = VInt::mask_gather<8>((uint8_t*)map.SectorMasks + 0, sectorIdx, mask);
    VInt mask_32 = VInt::mask_gather<8>((uint8_t*)map.SectorMasks + 4, sectorIdx, mask);

    VInt maskIdx = BrickIndexer::GetIndex(pos.x >> VoxelIndexer::ShiftXZ, pos.y >> VoxelIndexer::ShiftY, pos.z >> VoxelIndexer::ShiftXZ);
    VInt currMask = csel(maskIdx < 32, mask_0, mask_32);
    VMask level0 = (currMask >> (maskIdx & 31) & 1) != 0;
    VInt lod = 3;

    if (simd::any(level0)) {
        VInt cellIdx = (sectorIdx*(sizeof(Brick)*64) + maskIdx*sizeof(Brick))>>6;
        cellIdx += Level0Indexer::GetIndex(pos.x >> 2, pos.y >> 2, pos.z >> 2);

        set_if(level0, maskIdx, BrickIndexer::GetIndex(pos.x, pos.y, pos.z));
        set_if(level0, lod, 0);

        set_if(level0, mask_0, VInt::mask_gather<8>((uint8_t*)map.OccupancyStorage.get() + 0, cellIdx, mask & level0));
        set_if(level0, mask_32, VInt::mask_gather<8>((uint8_t*)map.OccupancyStorage.get() + 4, cellIdx, mask & level0));

        currMask = csel(maskIdx < 32, mask_0, mask_32);
        level0 = (currMask >> (maskIdx & 31) & 1) != 0;
    }

    VMask level4 = (mask_0 | mask_32) == 0;
    VMask level2 = (currMask >> (maskIdx & 0xA) & 0x00330033) == 0;
    lod += csel(level4, 2, csel(level2, 1, VInt(0)));

    VInt cellMask = (1 << lod) - 1;
    // TODO: can this be optimized to 1x ternlog using shift to get dir sign? bypass latency probably cheaper than cmp
    pos.x = csel(dir.x < 0, (pos.x & ~cellMask), (pos.x | cellMask));
    pos.y = csel(dir.y < 0, (pos.y & ~cellMask), (pos.y | cellMask));
    pos.z = csel(dir.z < 0, (pos.z & ~cellMask), (pos.z | cellMask));

    return level0;
}

// TODO: consider 2x unroll to help hide gather latency from better ILP
[[gnu::noinline]]
static HitInfo RayCast(const FlatVoxelStorage& map, VFloat3 origin, VFloat3 dir, VMask activeMask, glm::ivec3 worldOrigin) {
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

    for (uint32_t i = 0; i < 128; i++) {
        voxelPos = worldOrigin + VInt3(floor2i(currPos.x), floor2i(currPos.y), floor2i(currPos.z));
        
        activeMask &= GetInboundMask(voxelPos.x, voxelPos.y, voxelPos.z);
        VMask hitMask = GetStepPos(map, voxelPos, dir, activeMask);

        activeMask &= ~hitMask;
        if (!any(activeMask)) break;

        voxelPos -= worldOrigin;
        sideDist.x = csel(activeMask, tStart.x + conv2f(voxelPos.x) * invDir.x, sideDist.x);
        sideDist.y = csel(activeMask, tStart.y + conv2f(voxelPos.y) * invDir.y, sideDist.y);
        sideDist.z = csel(activeMask, tStart.z + conv2f(voxelPos.z) * invDir.z, sideDist.z);

        VFloat tmin = min(min(sideDist.x, sideDist.y), sideDist.z) + 0.001f;
        currPos = origin + tmin * dir;
    }

    VMask sideMaskX = sideDist.x < min(sideDist.y, sideDist.z);
    VMask sideMaskY = sideDist.y < min(sideDist.x, sideDist.z);
    VMask sideMaskZ = ~sideMaskX & ~sideMaskY;

    return {
        .MaterialData = GetVoxelMaterial(map, voxelPos, ~activeMask),
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
        .Mask = (VMask)(~activeMask),
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

    _blitShader = shlib.LoadFrag("BlitTiledFramebuffer_4x4", { { "FORMAT_RGB10", "1" } });

    _map->MarkAllDirty();
}

CpuRenderer::~CpuRenderer() = default;

struct Framebuffer {
    uint32_t Width, Height, TileStride;
    uint32_t Pixels[];

    uint32_t GetPixelOffset(uint32_t x, uint32_t y) {
        const uint32_t TileSize = 4, TileShift = 2, TileMask = TileSize - 1, TileNumPixels = TileSize * TileSize;

        uint32_t tileId = (x >> TileShift) + (y >> TileShift) * TileStride;
        uint32_t pixelOffset = (x & TileMask) + (y & TileMask) * TileSize;
        return tileId * TileNumPixels + pixelOffset;
    }
};

void CpuRenderer::RenderFrame(glim::Camera& cam, glm::uvec2 viewSize) {
#ifndef NDEBUG  // debug builds are slow af
    viewSize /= 4;
#endif
    viewSize &= ~3u;  // round down to 4x4 steps

    _storage->SyncBuffers(*_map);

    static glm::dvec3 prevCameraPos;
    static glm::quat prevCameraRot;
    bool camChanged = false;

    if (glm::distance(cam.ViewPosition, prevCameraPos) > 0.1f || glm::dot(cam.ViewRotation, prevCameraRot) < 0.999999f) {
        camChanged = true;
    }
    prevCameraPos = cam.ViewPosition;
    prevCameraRot = cam.ViewRotation;

    if (_pbo == nullptr || _pbo->Size < viewSize.y * viewSize.y * 4 + 12) {
        _pbo = std::make_unique<ogl::Buffer>(viewSize.x * viewSize.y * 4 + 12, GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT);
    }
    if (_accumTex == nullptr || _accumTex->Width != viewSize.x || _accumTex->Height != viewSize.y) {
        _accumTex = std::make_unique<ogl::Texture2D>(viewSize.x, viewSize.y, 1, GL_RGBA16F);
    }

    glm::dvec3 camPos = cam.ViewPosition;
    glm::ivec3 worldOrigin = glm::ivec3(glm::floor(camPos));

    glm::mat4 viewMat = glm::translate(cam.GetViewMatrix(false), glm::vec3(glm::floor(camPos) - camPos));
    glm::mat4 invProj = glm::inverse(cam.GetProjMatrix() * viewMat);
    // Bias matrix to take UVs in range [0..screen] rather than [-1..1]
    invProj = glm::translate(invProj, glm::vec3(-1.0f, -1.0f, 0.0f));
    invProj = glm::scale(invProj, glm::vec3(2.0f / viewSize.x, 2.0f / viewSize.y, 1.0f));

    auto fb = _pbo->Map<Framebuffer>(GL_MAP_WRITE_BIT);
    fb->Width = viewSize.x;
    fb->Height = viewSize.y;
    fb->TileStride = viewSize.x / 4;

    _frameTime.Begin();

    auto rows = std::ranges::iota_view(0u, (viewSize.y + 3) / 4);
    std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), [&](uint32_t rowId) {
        VRandom rng(rowId + _frameNo * 123456ull);
        uint32_t y = rowId * 4;

        for (uint32_t x = 0; x < viewSize.x; x += 4) {
            VFloat v = simd::conv2f((int32_t)y + FragPixelOffsetsY) + rng.NextUnsignedFloat() - 0.5f;
            VFloat u = simd::conv2f((int32_t)x + FragPixelOffsetsX) + rng.NextUnsignedFloat() - 0.5f;

            VFloat3 origin, dir;
            GetPrimaryRay({ u, v }, invProj, origin, dir);

            VFloat3 attenuation = 1.0f;
            VFloat3 finalColor = 0.0f;
            VMask mask = (VMask)(~0u);

            for (uint32_t i = 0; i < 3 && mask; i++) {
                auto hit = RayCast(*_storage, origin, dir, mask, worldOrigin);

                VFloat3 matColor = hit.GetColor();
                VFloat emissionStrength = hit.GetEmissionStrength();
/*
                VMask missMask = mask & ~hit.Mask;
                if (missMask) {
                    //constexpr SamplerDesc SD = { .MinFilter = FilterMode::Nearest, .EnableMips = true };
                    //VFloat3 skyColor = _skyBox->SampleCube<SD>(dir);
                    VFloat3 skyColor = { 0.6, 0.8, 0.96 };
                    matColor.x = simd::csel(missMask, skyColor.x, matColor.x);
                    matColor.y = simd::csel(missMask, skyColor.y, matColor.y);
                    matColor.z = simd::csel(missMask, skyColor.z, matColor.z);
                    emissionStrength = simd::csel(missMask, 1.0f, emissionStrength);
                }*/

                if (!_enablePathTracer) [[unlikely]] {
                    finalColor = matColor;
                    break;
                }

                attenuation *= matColor;
                finalColor += attenuation * emissionStrength;
                mask &= hit.Mask;

                origin = hit.Pos + hit.Normal * 0.01f;
                dir = simd::normalize(hit.Normal + rng.NextDirection());  // lambertian
            }
            VInt color = swr::pixfmt::RGB10u::Pack(finalColor * 0.25);
            color.store(&fb->Pixels[fb->GetPixelOffset(x, y)]);
        }
    });

    _frameTime.End();
    _frameNo++;

    _blitShader->SetUniform("ssbo_FrameData", *_pbo);
    _blitShader->SetUniform("u_AccumTex", *_accumTex);
    float blendWeight = camChanged ? 0.8f : 0.1f;
    _blitShader->SetUniform("u_BlendWeight", &blendWeight, 1);
    _blitShader->DispatchFullscreen();
}
void CpuRenderer::DrawSettings(glim::SettingStore& settings) {
    ImGui::SeparatorText("Renderer##CPU");
    settings.Checkbox("Path Trace", &_enablePathTracer);

    ImGui::Separator();
    _frameTime.Draw("Frame Time");
}

#endif // SIMD_AVX512