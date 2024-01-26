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
    uint8_t Data;

    bool IsEmpty() const { return Data == 0; }

    static Voxel CreateEmpty() {
        return { };
    }
    static Voxel Create(uint32_t materialId) {
        assert(materialId < 256);
        return { .Data = (uint8_t)materialId };
    }
};
struct Material {
    // 0..15    Color (RGB565)
    // 16..20   Emissive
    uint32_t Data;

    static Material CreateDiffuse(glm::vec3 color, float emissionStrength = 0.0f) {
        // TODO: proper hdr emission
        uint32_t emissionBits = glm::round(glm::clamp(emissionStrength / 7.0f, 0.0f, 1.0f) * 15.0f);
        return { .Data = PackRGB(color) | emissionBits << 16 };
    }

    static uint32_t PackRGB(glm::vec3 value) {
        value = glm::clamp(value, 0.0f, 1.0f);
        return (uint32_t)glm::round(value.x * 31.0f) << 11 |
               (uint32_t)glm::round(value.y * 63.0f) << 5 |
               (uint32_t)glm::round(value.z * 31.0f) << 0;
    }
};

struct VoxelPack {
    VInt Data;

    VMask IsEmpty() const { return Data == 0; }
};
struct MaterialPack {
    VInt Data;

    VFloat3 GetColor() const {
        return {
            conv2f((Data >> 11) & 31) * (1.0f / 31),
            conv2f((Data >> 5) & 63) * (1.0f / 63),
            conv2f((Data >> 0) & 31) * (1.0f / 31),
        };
    }
    VFloat GetEmissionStrength() const { return conv2f((Data >> 16) & 15) * (7.0f / 15); }
};

struct OccupancyMap {
    static const uint32_t NumLevels = 5;  // 1/32

    std::unique_ptr<uint32_t[]> Data;  // N x N x (N / 32)
    uint32_t CubeSize, CubeSizeLog2;
    uint32_t MipOffsets[NumLevels];

    OccupancyMap(uint32_t cubeSize) {
        uint32_t pos = 0;
        for (uint32_t i = 0; i < NumLevels; i++) {
            MipOffsets[i] = pos;

            uint32_t d = cubeSize >> i;
            pos += d * d * ((d + 31) / 32);
        }
        Data = std::make_unique<uint32_t[]>(pos);
        CubeSize = cubeSize;
        CubeSizeLog2 = (uint32_t)std::bit_width(cubeSize - 1);
        assert(CubeSize == (1 << CubeSizeLog2)); // must be pow2
    }

    // Update occupancy of a single brick located at the given voxel pos.
    void UpdateBrick(glm::uvec3 basePos, const Voxel* voxels);

    // Update lower mips for a NxNx32 region, starting at the given voxel pos.
    void UpdateMips(glm::uvec3 basePos, uint32_t planeSize);

    // Update occupancy of a single voxel.
    void Update(glm::uvec3 pos, bool occupied);

    uint32_t GetColumnIndex(glm::uvec3 pos, uint32_t level) const {
        uint32_t s = CubeSizeLog2 - level;
        assert((pos.x | pos.y | pos.z) < (1 << s));
        return MipOffsets[level] + pos.x + (pos.y << s) + ((pos.z >> 5) << (s * 2));
    }
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
    OccupancyMap OccMap { NumVoxelsPerAxis / 2 }; // occupancy map starts at double voxel size
    Material Palette[256] {};

    std::unique_ptr<ogl::Texture3D> GpuBrickStorage;
    std::unique_ptr<ogl::Texture3D> GpuOccupancyStorage;
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
        return brick ? brick->Data[GetVoxelIndex(pos.x, pos.y, pos.z)] : Voxel::CreateEmpty();
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
        return { .Data = data & 0xFF };
    }
    MaterialPack GetMaterial(VoxelPack voxels, VMask mask) const {
        VInt mat = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), mask, voxels.Data, Palette, 4);
        return { .Data = mat };
    }

    void SyncGpuBuffers();

    void Deserialize(std::string_view filename);
    void Serialize(std::string_view filename);

    void VoxelizeModel(const glim::Model& model, glm::uvec3 pos, glm::uvec3 size);

    template<typename TVisitor>
    void ForEach(TVisitor fn, bool skipEmptyBricks = false) {
        for (uint32_t i = 0; i < NumTotalBricks; i++) {
            if (skipEmptyBricks && !Storage->BrickSlots[i]) continue;

            glm::uvec3 brickPos = GetBrickPos(i) * BrickSize;

            for (uint32_t j = 0; j < NumVoxelsPerBrick; j++) {
                fn(brickPos + GetVoxelPos(j));
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

    static glm::uvec3 GetBrickPos(uint32_t index) {
        return {
            (index >> (BrickShift * 0)) & BrickMask,
            (index >> (BrickShift * 2)) & BrickMask,
            (index >> (BrickShift * 1)) & BrickMask,
        };
    }
    static glm::uvec3 GetVoxelPos(uint32_t index) {
        return {
            (index >> (BrickVoxelShift * 0)) & BrickVoxelMask,
            (index >> (BrickVoxelShift * 1)) & BrickVoxelMask,
            (index >> (BrickVoxelShift * 2)) & BrickVoxelMask,
        };
    }
};

}; // namespace cvox