#pragma once

#include <cstdint>
#include <map>
#include <glm/glm.hpp>

#include <Common/Scene.h>

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
struct alignas(16) Material {
    uint8_t Color[3] = { 0, 0, 0 };
    uint8_t MetalFuzziness = 255;
    float Emission = 0.0f;

    uint64_t GetEncoded() const {
        // Color (RGB565): u16
        // Emission:       f16
        // Fuzziness:      unorm8
        uint64_t packed = 0;

        packed |= (uint64_t)Color[0] >> (8 - 5) << 11;
        packed |= (uint64_t)Color[1] >> (8 - 6) << 5;
        packed |= (uint64_t)Color[2] >> (8 - 5) << 0;
        packed |= (uint64_t)glm::packHalf2x16(glm::vec2(0.0f, Emission));

        packed |= (uint64_t)MetalFuzziness << 32;

        return packed;
    }

    // Returns color normalized into 0..1 range
    glm::vec3 GetColor() const { return glm::vec3(Color[0], Color[1], Color[2]) * (1.0f / 255); }
    void SetColor(glm::vec3 value) {
        value = glm::clamp(value * 255.0f, 0.0f, 255.0f);
        Color[0] = glm::round(value.x);
        Color[1] = glm::round(value.y);
        Color[2] = glm::round(value.z);
    }
};

static uint32_t GetLinearIndex(glm::uvec3 pos, uint32_t sizeXZ, uint32_t sizeY) {
    assert(std::has_single_bit(sizeXZ) && std::has_single_bit(sizeY));
    return (pos.x & (sizeXZ - 1)) +
           (pos.z & (sizeXZ - 1)) * sizeXZ +
           (pos.y & (sizeY - 1)) * (sizeXZ * sizeXZ);
}

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

using WorldSectorIndexer = LinearIndexer3D<12, 8, true>;
using MaskIndexer = LinearIndexer3D<2, 2, false>;      // 4x4x4 64-bit masks
using BrickIndexer = LinearIndexer3D<3, 3, false>;

struct VoxelDispatchInvocationPars {
    VInt X, Y, Z;
    VInt VoxelIds;
    uint32_t GroupBaseIdx;
};

struct Brick {
    static constexpr glm::ivec3 Size = BrickIndexer::Size;

    Voxel Data[BrickIndexer::MaxArea] = {};

    bool IsEmpty() const;

    // Iterates over voxels within this brick.
    template<typename F>
    bool DispatchSIMD(F fn, glm::ivec3 basePos = {}) {
        bool dirty = false;
        VoxelDispatchInvocationPars p;

        for (uint32_t i = 0; i < BrickIndexer::MaxArea; i += VInt::Length) {
            static_assert(VInt::Length <= BrickIndexer::MaxArea);

            VInt vi = (int32_t)i + simd::RampI;
            p.X = (basePos.x * BrickIndexer::SizeXZ) + (vi & BrickIndexer::MaskXZ);
            p.Z = (basePos.z * BrickIndexer::SizeXZ) + (vi >> BrickIndexer::ShiftXZ & BrickIndexer::MaskXZ);
            p.Y = (basePos.y * BrickIndexer::SizeY) + (vi >> (BrickIndexer::ShiftXZ * 2));
            p.GroupBaseIdx = i;

#ifdef __AVX512F__
            p.VoxelIds = _mm512_cvtepu8_epi32(_mm_loadu_epi8(&Data[i]));
            if (fn(p)) {
                _mm_storeu_epi8(&Data[i], _mm512_cvtepi32_epi8(p.VoxelIds));
                dirty = true;
            }
#else
            p.VoxelIds = _mm256_cvtepu8_epi32(_mm_loadu_si64(&Data[i]));
            if (fn(p)) {
                auto tmp = _mm_packus_epi32(_mm256_extracti128_si256(p.VoxelIds, 0), _mm256_extracti128_si256(p.VoxelIds, 1));
                _mm_storeu_si64(&Data[i], _mm_packus_epi16(tmp, tmp));
                dirty = true;
            }
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
    static_assert(MaskIndexer::MaxArea == 64);

    std::vector<Brick> Storage;
    uint8_t BrickSlots[64]{};

    Brick* GetBrick(uint32_t index, bool create = false);
    // Bulk delete bricks indicated by mask
    void DeleteBricks(uint64_t mask);

    uint64_t GetAllocationMask();
    uint64_t DeleteEmptyBricks(uint64_t mask = ~0ull);

    static uint32_t GetBrickIndexFromSlot(uint64_t allocMask, uint32_t slotIdx);
};

struct HitResult {
    double Distance = -1.0;
    glm::vec3 Normal;
    glm::vec2 UV;
    glm::ivec3 VoxelPos;

    bool IsMiss() const { return Distance <= 0.0; }
};

struct VoxelMap {
    static constexpr glm::ivec3 MinPos = WorldSectorIndexer::MinPos * MaskIndexer::Size * BrickIndexer::Size;
    static constexpr glm::ivec3 MaxPos = WorldSectorIndexer::MaxPos * MaskIndexer::Size * BrickIndexer::Size;

    std::unordered_map<uint32_t, Sector> Sectors;
    std::map<uint32_t, uint64_t> DirtyLocs;         // 4x4x4 masks of dirty bricks

    Material Palette[256] {};

    Brick* GetBrick(glm::ivec3 pos, bool create = false, bool markAsDirty = false);

    Voxel Get(glm::ivec3 pos) {
        Brick* brick = GetBrick(pos >> BrickIndexer::Shift);
        return brick ? brick->Data[BrickIndexer::GetIndex(pos)] : Voxel::CreateEmpty();
    }
    void Set(glm::ivec3 pos, Voxel voxel) {
        Brick* brick = GetBrick(pos >> BrickIndexer::Shift, true, true);
        if (brick == nullptr) return; // out of bounds

        uint32_t idx = BrickIndexer::GetIndex(pos);
        brick->Data[idx] = voxel;
    }
    static bool CheckInBounds(glm::ivec3 pos) {
        pos >>= (BrickIndexer::Shift + MaskIndexer::Shift);
        return WorldSectorIndexer::CheckInBounds(pos);
    }

    void MarkAllDirty() {
        for (auto& [idx, sector] : Sectors) {
            DirtyLocs[idx] = sector.GetAllocationMask();
        }
    }

    // Slow scalar raycaster intended for mouse picking and stuff.
    HitResult RayCast(glm::dvec3 origin, glm::dvec3 dir, uint32_t maxIters = 1024);

    void Deserialize(std::string_view filename);
    void Serialize(std::string_view filename);

    void VoxelizeModel(const glim::Model& model, glm::uvec3 pos, glm::uvec3 size);

    // Iterates over bricks within the specified region (in voxel coords).
    template<typename F>
    void RegionDispatchSIMD(glm::ivec3 regionMin, glm::ivec3 regionMax, bool createEmpty, F fn) {
        glm::ivec3 brickMin = glm::max(regionMin >> glm::ivec3(BrickIndexer::Shift), MinPos);
        glm::ivec3 brickMax = glm::min(regionMax >> glm::ivec3(BrickIndexer::Shift), MaxPos);

        std::unordered_map<uint32_t, uint64_t> emptyBricks;

        for (int32_t by = brickMin.y; by <= brickMax.y; by++) {
            for (int32_t bz = brickMin.z; bz <= brickMax.z; bz++) {
                for (int32_t bx = brickMin.x; bx <= brickMax.x; bx++) {
                    glm::ivec3 brickPos = glm::ivec3(bx, by, bz);
                    Brick* brick = GetBrick(brickPos, createEmpty);
                    if (brick == nullptr) continue;

                    bool changed = brick->DispatchSIMD(fn, brickPos);
                    bool isEmpty = brick->IsEmpty();

                    if (changed || isEmpty) {
                        uint32_t sectorIdx = WorldSectorIndexer::GetIndex(brickPos >> MaskIndexer::Shift);
                        uint64_t brickMask = 1ull << MaskIndexer::GetIndex(brickPos);

                        DirtyLocs[sectorIdx] |= brickMask;

                        if (isEmpty) {
                            emptyBricks[sectorIdx] |= brickMask;
                        }
                    }
                }
            }
        }

        // Garbage collect
        for (auto [sectorIdx, emptyMask] : emptyBricks) {
            Sector& sector = Sectors[sectorIdx];

            if ((sector.GetAllocationMask() & ~emptyMask) != 0) {
                sector.DeleteBricks(emptyMask);
            } else {
                Sectors.erase(sectorIdx);
            }
        }
    }
};
