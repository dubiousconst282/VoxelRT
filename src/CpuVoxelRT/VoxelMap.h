#pragma once

#include <cstdint>
#include <Common/Scene.h>

#include <map>

struct Voxel {
    uint8_t Data = 0;

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

template<int ShiftXZ_, int ShiftY_, bool Signed_>
struct LinearIndexer3D {
    static const int32_t ShiftXZ = ShiftXZ_, ShiftY = ShiftY_;
    static const int32_t SizeXZ = 1 << ShiftXZ, SizeY = 1 << ShiftY;
    static const int32_t MaskXZ = SizeXZ - 1, MaskY = SizeY - 1;
    static const size_t MaxArea = (size_t)(1ull << (ShiftXZ * 2 + ShiftY));

    static constexpr glm::ivec3 Size = glm::ivec3(SizeXZ, SizeY, SizeXZ);
    static constexpr glm::ivec3 Shift = glm::ivec3(ShiftXZ, ShiftY, ShiftXZ);

    static constexpr glm::ivec3 MinPos = Signed_ ? -(Size / 2) : glm::ivec3(0);
    static constexpr glm::ivec3 MaxPos = Signed_ ? (Size / 2 - 1) : Size;

    static bool CheckInBounds(glm::ivec3 pos) {
        if (Signed_) pos += Size / 2;
        return uint32_t(pos.x | pos.z) < SizeXZ && uint32_t(pos.y) < SizeY;
    }
    static uint32_t GetIndex(glm::ivec3 pos) {
        return (uint32_t)GetIndex(pos.x, pos.y, pos.z);
    }
    static glm::ivec3 GetPos(uint32_t index) {
        if constexpr (Signed_) {
            int32_t x = int32_t(index << (32 - ShiftXZ)) >> (32 - ShiftXZ);
            int32_t z = int32_t(index << (32 - ShiftXZ * 2)) >> (32 - ShiftXZ);
            int32_t y = int32_t(index << (32 - ShiftXZ * 2 - ShiftY)) >> (32 - ShiftY);
            return { x, y, z };
        } else {
            int32_t x = index & MaskXZ;
            int32_t z = index >> ShiftXZ & MaskXZ;
            int32_t y = index >> (ShiftXZ * 2) & MaskY;
            return { x, y, z };
        }
    }

    template<typename T>
    static T GetIndex(T x, T y, T z) {
        return (x & MaskXZ) | (z & MaskXZ) << ShiftXZ | (y & MaskY) << (ShiftXZ * 2);
    }
};

using SectorIndexer = LinearIndexer3D<12, 8, true>;     // Map -> Sector
using BrickIndexer = LinearIndexer3D<2, 2, false>;      // Sector -> Brick
using VoxelIndexer = LinearIndexer3D<3, 3, false>;      // Brick -> Voxel

struct VoxelDispatchInvocationPars {
    swr::VInt X, Y, Z;
    swr::VInt VoxelIds;
    uint32_t GroupBaseIdx;
};

struct Brick {
    static constexpr glm::ivec3 Size = VoxelIndexer::Size;

    Voxel Data[VoxelIndexer::MaxArea] = {};

    bool IsEmpty() const;

    // Iterates over voxels within this brick.
    template<typename F>
    bool DispatchSIMD(F fn, glm::ivec3 basePos = {}) {
        using namespace swr::simd;

        bool dirty = false;
        VoxelDispatchInvocationPars p;

        for (uint32_t i = 0; i < VoxelIndexer::MaxArea; i += VInt::Length) {
            static_assert(VInt::Length <= VoxelIndexer::MaxArea);

            VInt vi = (int32_t)i + VInt::ramp();
            p.X = (basePos.x * VoxelIndexer::SizeXZ) + (vi & VoxelIndexer::MaskXZ);
            p.Z = (basePos.z * VoxelIndexer::SizeXZ) + (vi >> VoxelIndexer::ShiftXZ & VoxelIndexer::MaskXZ);
            p.Y = (basePos.y * VoxelIndexer::SizeY) + (vi >> (VoxelIndexer::ShiftXZ * 2));
            p.GroupBaseIdx = i;

#ifdef __AVX512F__
            p.VoxelIds = _mm512_cvtepu8_epi32(_mm_loadu_epi8(&Data[i]));
            if (fn(p)) {
                _mm_storeu_epi8(&Data[i], _mm512_cvtepi32_epi8(p.VoxelIds));
                dirty = true;
            }
#elif __AVX2__
            p.VoxelIds = _mm256_cvtepu8_epi32(_mm_loadu_si64(&Data[i]));
            if (fn(p)) {
                _mm_storeu_si64(&Data[i], _mm256_cvtepi32_epi8(p.VoxelIds));
                dirty = true;
            }
#else
#error SIMD accel not supported in this compiler or platform
#endif
        }
        return dirty;
    }
};

// 4x4x4 region of bricks.
// TODO: consider implementing bit-packing: 1/2/4/8 bits per voxel
//      - makes accesses difficult, need Get/Set, Gather/Scatter APIs
//      - makes palette sharing difficult, but sector is 32³ so global sharing might still be reasonable
struct Sector {
    static_assert(BrickIndexer::MaxArea == 64);

    std::vector<Brick> Storage;
    uint8_t BrickSlots[64]{};

    Brick* GetBrick(uint32_t index, bool create = false);
    // Bulk delete bricks indicated by mask
    void DeleteBricks(uint64_t mask);

    uint64_t GetAllocationMask();
    uint64_t DeleteEmptyBricks(uint64_t mask = ~0ull);

    static uint32_t GetBrickIndexFromSlot(uint64_t allocMask, uint32_t slotIdx);
};

struct VoxelMap {
    static constexpr glm::ivec3 MinPos = SectorIndexer::MinPos * BrickIndexer::Size * VoxelIndexer::Size;
    static constexpr glm::ivec3 MaxPos = SectorIndexer::MaxPos * BrickIndexer::Size * VoxelIndexer::Size;

    std::unordered_map<uint32_t, Sector> Sectors;
    std::map<uint32_t, uint64_t> DirtyLocs;         // 4x4x4 masks of dirty bricks

    Material Palette[256] {};

    Brick* GetBrick(glm::ivec3 pos, bool create = false, bool markAsDirty = false);

    Voxel Get(glm::ivec3 pos) {
        Brick* brick = GetBrick(pos >> VoxelIndexer::Shift);
        return brick ? brick->Data[VoxelIndexer::GetIndex(pos)] : Voxel::CreateEmpty();
    }
    void Set(glm::ivec3 pos, Voxel voxel) {
        Brick* brick = GetBrick(pos >> VoxelIndexer::Shift, true, true);
        if (brick == nullptr) return; // out of bounds

        uint32_t idx = VoxelIndexer::GetIndex(pos);
        brick->Data[idx] = voxel;
    }
    static bool CheckInBounds(glm::ivec3 pos) {
        pos >>= (VoxelIndexer::Shift + BrickIndexer::Shift);
        return SectorIndexer::CheckInBounds(pos);
    }

    void MarkAllDirty() {
        for (auto& [idx, sector] : Sectors) {
            DirtyLocs[idx] |= sector.GetAllocationMask();
        }
    }
    // Gets and unmarks the position of the next dirty brick.
    bool PopDirty(glm::ivec3& pos);

    // Slow scalar raycaster intended for picking and stuff.
    // Returns hit distance or -1 if no hit was found.
    double RayCast(glm::dvec3 origin, glm::vec3 dir, uint32_t maxIters);

    void Deserialize(std::string_view filename);
    void Serialize(std::string_view filename);

    void VoxelizeModel(const glim::Model& model, glm::uvec3 pos, glm::uvec3 size);

    // Iterates over bricks within the specified region (in voxel coords).
    template<typename F>
    void RegionDispatchSIMD(glm::ivec3 regionMin, glm::ivec3 regionMax, bool createEmpty, F fn) {
        using namespace swr::simd;

        glm::ivec3 brickMin = glm::max(regionMin >> glm::ivec3(VoxelIndexer::Shift), MinPos);
        glm::ivec3 brickMax = glm::min(regionMax >> glm::ivec3(VoxelIndexer::Shift), MaxPos);

        for (int32_t by = brickMin.y; by <= brickMax.y; by++) {
            for (int32_t bz = brickMin.z; bz <= brickMax.z; bz++) {
                for (int32_t bx = brickMin.x; bx <= brickMax.x; bx++) {
                    glm::ivec3 brickPos = glm::ivec3(bx, by, bz);
                    Brick* brick = GetBrick(brickPos, createEmpty);

                    if (brick != nullptr && brick->DispatchSIMD(fn, brickPos)) {
                        uint32_t sectionIdx = SectorIndexer::GetIndex(brickPos >> BrickIndexer::Shift);
                        DirtyLocs[sectionIdx] |= 1ull << BrickIndexer::GetIndex(brickPos);
                    }
                }
            }
        }
    }
};
