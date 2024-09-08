#pragma once

#include <cstdint>
#include <vector>
#include <map>
#include <unordered_map>
#include <string_view>

#include <glm/glm.hpp>
#include <SwRast/SIMD.h>

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
    static uint32_t GetIndex(glm::ivec3 pos) { return (uint32_t)GetIndex(pos.x, pos.y, pos.z); }
    
    template<typename T>
    static T GetIndex(T x, T y, T z) {
        return (x & MaskXZ) | (z & MaskXZ) << ShiftXZ | (y & MaskY) << (ShiftXZ * 2);
    }
    template<typename Vec = glm::ivec3, typename T>
    static Vec GetPos(T index) {
        T x = index & MaskXZ;
        T z = index >> ShiftXZ & MaskXZ;
        T y = index >> (ShiftXZ * 2) & MaskY;

        if constexpr (Signed_) {
            x = signext<ShiftXZ>(x);
            z = signext<ShiftXZ>(z);
            y = signext<ShiftY>(y);
        }
        return { x, y, z };
    }

private:
    template<int bits, typename T>
    static T signext(T value) {
        // The classic way to sign extend is `sint(value << (32 - bits)) >> (32 - bits)`,
        // but that is not trivial to do in a generic context because we can't easily do
        // arithmetic bit shifts.
        //
        // The following is comparatively as cheap, but in the case of SIMD will require
        // loading a constant from memory.
        constexpr T sign_mask = T(1) << (bits - 1);
        return (value ^ sign_mask) - sign_mask;
    }
};

using WorldSectorIndexer = LinearIndexer3D<12, 8, true>;
using MaskIndexer = LinearIndexer3D<2, 2, false>;      // 4x4x4 64-bit masks
using BrickIndexer = LinearIndexer3D<3, 3, false>;

struct Brick {
    static constexpr glm::ivec3 Size = BrickIndexer::Size;
    static constexpr uint32_t NumVoxels = BrickIndexer::MaxArea;

    Voxel Data[NumVoxels] = {};

    bool IsEmpty() const;
    void GenerateLOD(Voxel* dest, uint32_t level) const;

    struct DispatchResult {
        bool Changed = false;
        bool Empty = false;
    };
    // Iterates over voxels within this brick.
    template<typename F>
    DispatchResult DispatchSIMD(F fn, glm::ivec3 basePos = {}) {
        static_assert(simd::VectorWidth <= NumVoxels);
        static_assert(NumVoxels % simd::VectorWidth == 0);

        DispatchResult result = { .Empty = true };

        for (int32_t i = 0; i < NumVoxels; i += simd::VectorWidth) {
            VInt3 pos = (basePos * BrickIndexer::Size) + BrickIndexer::GetPos<VInt3>(i + simd::LaneIdx);

#if SIMD_AVX512
            VInt currIds = _mm512_cvtepu8_epi32(_mm_loadu_epi8(&Data[i]));
            VInt newIds = currIds;
            fn(pos, newIds);
            if (simd::any(currIds != newIds)) {
                _mm_storeu_epi8(&Data[i], _mm512_cvtepi32_epi8(newIds));
                result.Changed = true;
            }
#else
            VInt currIds = _mm256_cvtepu8_epi32(_mm_loadu_si64(&Data[i]));
            VInt newIds = currIds;
            fn(pos, newIds);
            if (simd::any(currIds != newIds)) {
                auto tmp = _mm_packus_epi32(_mm256_extracti128_si256(newIds, 0), _mm256_extracti128_si256(newIds, 1));
                _mm_storeu_si64(&Data[i], _mm_packus_epi16(tmp, tmp));
                result.Changed = true;
            }
#endif
            result.Empty &= simd::all(newIds == 0);
        }
        return result;
    }

    // Note: indexing is brick-wise linear XZY, not tiled.
    static void GetOccupancyMask(const Brick* brick, uint64_t dest[NumVoxels / 64]) {
        static_assert(sizeof(Voxel) == 1);

        if (!brick) {
            memset(dest, 0, NumVoxels / 8);
            return;
        }

        for (uint32_t i = 0; i < NumVoxels; i += 64) {
            dest[i / 64] = PackBits64((uint8_t*)&brick->Data[i]);
        }
    }

    // Create 64-bit mask of `data[i] != 0`
    static uint64_t PackBits64(const uint8_t data[64]) {
#if __AVX512F__
        uint64_t mask = _mm512_cmpneq_epi8_mask(_mm512_loadu_epi8(data), _mm512_set1_epi8(0));
#elif __AVX2__
        uint64_t mask = ~0ull;
        for (uint32_t i = 0; i < 64; i += 32) {
            __m256i v = _mm256_loadu_si256((__m256i*)&data[i]);
            v = _mm256_cmpeq_epi8(v, _mm256_set1_epi8(0));
            mask ^= uint64_t(uint32_t(_mm256_movemask_epi8(v))) << i;
        }
#else
        uint64_t mask = 0;
        for (uint32_t i = 0; i < 64; i++) {
            uint64_t bit = data[i] != 0;
            mask |= bit << i;
        }
#endif
        return mask;
    }
};

// 4x4x4 region of bricks.
struct Sector {
    static_assert(MaskIndexer::MaxArea == 64);

    std::vector<Brick> Storage;
    uint8_t BrickSlots[64]{};

    Brick* GetBrick(uint32_t index, bool create = false);
    // Bulk delete bricks indicated by mask
    void DeleteBricks(uint64_t mask);

    uint64_t GetAllocationMask() const {
        static_assert(sizeof(BrickSlots) == 64);
        return Brick::PackBits64(BrickSlots);
    }
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

// 3D for loop within given cube bounds, in YZX order
#define for_yzx_inclusive(v_type, name, min, max) \
    for (auto name##__y = (min).y; name##__y <= (max).y; name##__y++) \
    for (auto name##__z = (min).z; name##__z <= (max).z; name##__z++) \
    for (auto name##__x = (min).x; name##__x <= (max).x; name##__x++) \
        if (auto name = v_type(name##__x, name##__y, name##__z); true)

struct VoxelMap {
    static constexpr glm::ivec3 MinPos = WorldSectorIndexer::MinPos * MaskIndexer::Size * BrickIndexer::Size;
    static constexpr glm::ivec3 MaxPos = WorldSectorIndexer::MaxPos * MaskIndexer::Size * BrickIndexer::Size;

    std::unordered_map<uint32_t, Sector> Sectors;
    std::map<uint32_t, uint64_t> DirtyLocs;         // 4x4x4 masks of dirty bricks

    Material Palette[256] {};

    Sector* GetSector(glm::ivec3 pos, bool create = false, uint64_t markAsDirty = 0);
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

    void VoxelizeModel(std::string_view modelPath, glm::ivec3 pos, glm::ivec3 size);

    // Iterates over voxels within the specified region (in voxel coords).
    template<typename VisitFn, typename FilterFn>
    void RegionDispatchSIMD(glm::ivec3 regionMin, glm::ivec3 regionMax, bool createEmpty,
                            VisitFn visitFn,
                            FilterFn filterFn = [](glm::ivec3 pos, Sector& sector) { return sector.GetAllocationMask(); }) {
        glm::ivec3 sectorMin = glm::max(regionMin >> (BrickIndexer::Shift + MaskIndexer::Shift), MinPos);
        glm::ivec3 sectorMax = glm::min(regionMax >> (BrickIndexer::Shift + MaskIndexer::Shift), MaxPos);

        for_yzx_inclusive(glm::ivec3, sectorPos, sectorMin, sectorMax) {
            uint32_t sectorIdx = WorldSectorIndexer::GetIndex(sectorPos);

            if (!createEmpty && !Sectors.contains(sectorIdx)) continue;
            Sector& sector = Sectors[sectorIdx];

            uint64_t visitMask = filterFn(sectorPos, sector);
            uint64_t dirtyMask = 0, emptyMask = 0;
            uint64_t initialAllocMask = sector.GetAllocationMask();

            for (uint32_t brickIdx : BitIter(visitMask)) {
                Brick* brick = sector.GetBrick(brickIdx, createEmpty);
                if (brick == nullptr) continue;

                auto info = brick->DispatchSIMD(visitFn, sectorPos * MaskIndexer::Size + MaskIndexer::GetPos(brickIdx));

                if (info.Changed) dirtyMask |= (1ull << brickIdx);
                if (info.Empty) emptyMask |= (1ull << brickIdx);
            }

            // Garbage collect
            if ((sector.GetAllocationMask() & ~emptyMask) == 0) {
                Sectors.erase(sectorIdx);
            } else if (emptyMask != 0) {
                sector.DeleteBricks(emptyMask);
            }

            if (dirtyMask != 0 || (emptyMask & initialAllocMask) != 0) {
                DirtyLocs[sectorIdx] |= dirtyMask;
            }
        }
    }
};
