#pragma once

#include <cstdint>
#include <memory>
#include <algorithm>
#include <set>

#include <SwRast/SIMD.h>
#include <Common/Scene.h>
#include <OGL/QuickGL.h>

namespace cvox {

using namespace swr::simd;

struct Voxel {
    uint8_t Packed;

    bool IsEmpty() const { return Packed < 32; }
    float GetDistToNearest() const { return IsEmpty() ? Packed : 0; }

    static Voxel CreateEmpty(float distToNearest) {
        int32_t scaledDist = std::clamp((int32_t)distToNearest, 0, 31);
        return { .Packed = (uint8_t)scaledDist };
    }
    static Voxel Create(uint32_t materialId) {
        assert(materialId < 256 - 32);
        return { .Packed = (uint8_t)(materialId + 32) };
    }
};
struct Material {
    // 0..15    Color (RGB565)
    // 16..20   Emissive
    uint32_t Packed;

    static Material CreateDiffuse(glm::vec3 color, float emissionStrength = 0.0f) {
        // TODO: proper hdr emission
        uint32_t emissionBits = glm::round(glm::clamp(emissionStrength / 7.0f, 0.0f, 1.0f) * 15.0f);
        return { .Packed = PackRGB(color) | emissionBits << 16 };
    }

    static uint32_t PackRGB(glm::vec3 value) {
        value = glm::clamp(value, 0.0f, 1.0f);
        return (uint32_t)glm::round(value.x * 31.0f) << 11 |
               (uint32_t)glm::round(value.y * 63.0f) << 5 |
               (uint32_t)glm::round(value.z * 31.0f) << 0;
    }
};

struct VoxelPack {
    VInt Packed;

    VMask IsEmpty() const { return Packed < 32; }
    VFloat GetDistToNearest() const { return csel(IsEmpty(), conv2f(Packed), 0); }
    VInt GetMaterialId() const { return csel(IsEmpty(), 0, Packed - 32); }
};
struct MaterialPack {
    VInt Packed;

    VFloat3 GetColor() const {
        return {
            conv2f((Packed >> 11) & 31) * (1.0f / 31),
            conv2f((Packed >> 5) & 63) * (1.0f / 63),
            conv2f((Packed >> 0) & 31) * (1.0f / 31),
        };
    }
    VFloat GetEmissionStrength() const { return conv2f((Packed >> 16) & 15) * (7.0f / 15); }
};
struct VoxelMap {
    static const uint32_t BrickShift = 7, BrickVoxelShift = 3;
    static const uint32_t NumBricksPerAxis = 1 << BrickShift, NumTotalBricks = 1 << (BrickShift * 3);
    static const uint32_t BrickSize = 1 << BrickVoxelShift, NumVoxelsPerBrick = 1 << (BrickVoxelShift * 3);
    static const uint32_t BrickMask = NumBricksPerAxis - 1, BrickVoxelMask = BrickSize - 1;
    static const uint32_t NumVoxelsPerAxis = NumBricksPerAxis * BrickSize;

    struct Brick {
        Voxel Data[NumVoxelsPerBrick];
    };
    std::unique_ptr<uint32_t[]> BrickSlots;
    std::vector<Brick> BrickStorage;
    Material Palette[256 - 32] {};

    std::unique_ptr<ogl::Texture3D> GpuBrickStorage;
    std::unique_ptr<ogl::Buffer> GpuMetaStorage;
    std::set<uint32_t> DirtyBricks;

    struct GpuMeta {
        Material Palette[sizeof(VoxelMap::Palette) / sizeof(Material)];
        uint32_t BrickSlots[NumTotalBricks]; // XYZ x 10-bit
    };

    VoxelMap() {
        BrickSlots = std::make_unique<uint32_t[]>(NumTotalBricks);

        BrickStorage.reserve(NumTotalBricks / 32);
        // Reserve empty brick at the 0th slot to simplify queries (bounds / null checks / subtracts).
        BrickStorage.emplace_back();
    }

    Brick* GetBrick(glm::uvec3 pos, bool markAsDirty = false, bool create = false) {
        assert((pos.x | pos.y | pos.z) < NumVoxelsPerAxis);

        uint32_t idx = GetBrickIndex(pos.x, pos.y, pos.z);
        uint32_t& slot = BrickSlots[idx];
        if (!slot) {
            if (!create) {
                return nullptr;
            }
            slot = BrickStorage.size();
            BrickStorage.emplace_back();
            markAsDirty = true;
        }
        if (markAsDirty) {
            DirtyBricks.insert(idx);
        }
        return &BrickStorage[slot];
    }

    Voxel Get(glm::uvec3 pos) {
        Brick* brick = GetBrick(pos);
        return brick ? brick->Data[GetVoxelIndex(pos.x, pos.y, pos.z)] : Voxel::CreateEmpty(1.0f);
    }
    void Set(glm::uvec3 pos, Voxel voxel) {
        Brick* brick = GetBrick(pos, true, true);

        uint32_t idx = GetVoxelIndex(pos.x, pos.y, pos.z);
        brick->Data[idx] = voxel;
    }

    // Creates mask for voxel coords that are inside the map.
    static VMask GetInboundMask(VInt x, VInt y, VInt z) {
        return _mm512_cmplt_epu32_mask(x | y | z, _mm512_set1_epi32(NumVoxelsPerAxis));
    }

    VoxelPack GetPack(VInt x, VInt y, VInt z, VMask mask) const {
        assert((mask & ~GetInboundMask(x, y, z)) == 0);

        VInt brickIdx = csel(mask, GetBrickIndex(x, y, z), 0);
        VInt brickSlot = _mm512_mask_i32gather_epi32(brickIdx, mask, brickIdx, BrickSlots.get(), 4);
        VInt storageOffset = brickSlot * sizeof(Brick) + GetVoxelIndex(x, y, z);

        VInt data = _mm512_mask_i32gather_epi32(_mm512_set1_epi32(0), mask, storageOffset, BrickStorage.data(), 1);
        return { .Packed = data & 0xFF };
    }
    MaterialPack GetMaterial(VoxelPack voxels) const {
        VMask mask = ~voxels.IsEmpty();
        VInt mat = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), mask, voxels.GetMaterialId(), Palette, 4);
        return { .Packed = mat };
    }

    void SyncGpuBuffers();

    void Deserialize(std::string_view filename);
    void Serialize(std::string_view filename);

    void VoxelizeModel(const scene::Model& model, glm::uvec3 pos, glm::uvec3 size);

    template<typename TVisitor>
    void ForEach(TVisitor fn, bool skipEmptyBricks = false) {
        for (uint32_t i = 0; i < NumTotalBricks; i++) {
            if (skipEmptyBricks && !Storage->BrickSlots[i]) continue;

            uint32_t bx = (i >> (BrickShift * 0)) & BrickMask;
            uint32_t bz = (i >> (BrickShift * 1)) & BrickMask;
            uint32_t by = (i >> (BrickShift * 2)) & BrickMask;

            for (uint32_t j = 0; j < NumVoxelsPerBrick; j++) {
                uint32_t vx = (j >> (BrickVoxelShift * 0)) & BrickVoxelMask;
                uint32_t vy = (j >> (BrickVoxelShift * 1)) & BrickVoxelMask;
                uint32_t vz = (j >> (BrickVoxelShift * 2)) & BrickVoxelMask;
                fn((bx << BrickVoxelShift) + vx, (by << BrickVoxelShift) + vy, (bz << BrickVoxelShift) + vz);
            }
        }
    }
    
    template<typename T>
    static T GetBrickIndex(T x, T y, T z) {
        return (y >> BrickVoxelShift) << (BrickShift * 2) |
               (z >> BrickVoxelShift) << (BrickShift * 1) |
               (x >> BrickVoxelShift) << (BrickShift * 0);
    }

    template<typename T>
    static T GetVoxelIndex(T x, T y, T z) {
        // OpenGL convention is XYZ
        return (z & BrickVoxelMask) << (BrickVoxelShift * 2) |
               (y & BrickVoxelMask) << (BrickVoxelShift * 1) |
               (x & BrickVoxelMask) << (BrickVoxelShift * 0);
    }
};

}; // namespace cvox