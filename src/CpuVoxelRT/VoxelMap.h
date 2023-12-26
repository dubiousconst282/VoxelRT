#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <SwRast/SIMD.h>

namespace cvox {

using namespace swr::simd;

struct Voxel {
    uint8_t Packed;

    bool IsEmpty() const { return (Packed & 0x80) == 0; }
    float GetDistToNearest() const { return IsEmpty() ? (Packed & 0x7F) / 4.0f : 0; }

    void SetEmpty(float distToNearest) { Packed = std::clamp((int32_t)(distToNearest * 4.0f), 0, 0x7F); }
    void SetMaterialId(uint32_t id) {
        assert(id < 128);
        Packed = id | 0x80;
    }
};
struct Material {
    // 0..11    Color (RGB444)
    // 12..16   Emissive
    uint32_t Packed;

    void SetColor(glm::vec3 value) { Packed = (Packed & ~0xFFFu) | PackRGB(value); }
    void SetEmissionStrength(float value) {
        // TODO: HDR?
        value = glm::round(glm::clamp(value, 0.0f, 1.0f) * 15.0f);
        Packed = (Packed & ~(0xFu << 12)) | ((uint32_t)value << 12);
    }

    static uint32_t PackRGB(glm::vec3 value) {
        value = glm::round(glm::clamp(value, 0.0f, 1.0f) * 15.0f);
        return (uint32_t)value.x << 8 | (uint32_t)value.y << 4 | (uint32_t)value.z << 0;
    }
};

struct VoxelPack {
    VInt Packed;

    VMask IsEmpty() const { return (Packed & 0x80) == 0; }
    VFloat GetDistToNearest() const { return csel(IsEmpty(), conv2f(GetData()) / 4.0f, 0); }

    VInt GetData() const { return Packed & 0x7F; }
};
struct MaterialPack {
    VInt Packed;

    VFloat3 GetColor() const {
        const float scale = 1.0f / 15;
        return {
            conv2f((Packed >> 8) & 15) * scale,
            conv2f((Packed >> 4) & 15) * scale,
            conv2f((Packed >> 0) & 15) * scale,
        };
    }
    VFloat GetEmissionStrength() const { return conv2f((Packed >> 12) & 15) * (1.0f / 15); }
};

struct VoxelMap {
    static const uint32_t Shift = 7, Size = 1 << Shift, Count = Size * Size * Size;
    std::unique_ptr<Voxel[]> Voxels;
    Material Palette[128]{};

    VoxelMap() { Voxels = std::make_unique<Voxel[]>(Count); }

    Voxel& At(uint32_t x, uint32_t y, uint32_t z) {
        assert((x | y | z) < Size);
        return Voxels[GetIndex(x, y, z)];
    }

    VoxelPack GetPack(VInt x, VInt y, VInt z) const {
        auto index = GetIndex(x, y, z);

        auto mask = _mm512_cmplt_epu32_mask(x | y | z, _mm512_set1_epi32(Size));
        auto emptyId = _mm512_set1_epi16(0);
        auto data = _mm512_mask_i32gather_epi32(emptyId, mask, index, Voxels.get(), 1);
        return { .Packed = VInt(data) & 0xFF };
    }
    MaterialPack GetMaterial(VoxelPack voxels) const {
        VMask mask = ~voxels.IsEmpty();
        VInt mat = _mm512_mask_i32gather_epi32(_mm512_setzero_si512(), mask, voxels.GetData(), Palette, 4);
        return { .Packed = mat };
    }

    template<typename TVisitor>
    void ForEach(TVisitor fn) {
        for (uint32_t y = 0; y < Size; y++) {
            for (uint32_t z = 0; z < Size; z++) {
                for (uint32_t x = 0; x < Size; x++) {
                    fn(x, y, z);
                }
            }
        }
    }

    void UpdateDistanceField() {
        auto distField = std::make_unique<int32_t[]>(Count);

        ForEach([&](uint32_t x, uint32_t y, uint32_t z) {
            Voxel& voxel = At(x, y, z);

            if (voxel.IsEmpty()) {
                distField[GetIndex(x, y, z)] = DF_INF;
            }
        });

        // X
        for (uint32_t i = 0; i < Size; i++) {
            for (uint32_t j = 0; j < Size; j++) {
                EuclideanDistanceTransform(&distField[i * (Size * Size) + j * Size], 1, Size);
            }
        }

        // Z
        for (uint32_t i = 0; i < Size; i++) {
            for (uint32_t j = 0; j < Size; j++) {
                EuclideanDistanceTransform(&distField[i * (Size * Size) + j], Size, Size);
            }
        }

        // Y
        for (uint32_t i = 0; i < Size; i++) {
            for (uint32_t j = 0; j < Size; j++) {
                EuclideanDistanceTransform(&distField[i * Size + j], Size * Size, Size);
            }
        }

        ForEach([&](uint32_t x, uint32_t y, uint32_t z) {
            Voxel& voxel = At(x, y, z);
            if (voxel.IsEmpty()) {
                voxel.SetEmpty(sqrtf(distField[GetIndex(x, y, z)]));
            }
        });
    }

private:
    template<typename T>
    static T GetIndex(T x, T y, T z) {
        return y * (Size * Size) + z * Size + x;
    }
    template<typename T>
    static T GetTiledIndex(T x, T y, T z) {
        const uint32_t S = Size / 4;

        T subIdx = (y & 3) * 16 + (z & 3) * 4 + (x & 3);
        T greatIdx = (y >> 2) * (S * S) + (z >> 2) * S + (x >> 2);
        return subIdx + greatIdx * 64;
    }

    static const int32_t DF_INF = (1 << 15);

    // Blatantly copied from https://acko.net/blog/subpixel-distance-transform/
    // https://cs.brown.edu/people/pfelzens/papers/dt-final.pdf
    static void EuclideanDistanceTransform(int32_t* Df, uint32_t stride, uint32_t n) {
        int32_t v[n], z[n + 1], f[n];
        v[0] = 0;
        z[0] = -DF_INF;
        z[1] = +DF_INF;
        f[0] = Df[0];

        int32_t k = 0;
        for (int32_t q = 1; q < n; q++) {
            f[q] = Df[q * (int32_t)stride];

            int32_t s;
            do {
                int32_t r = v[k];
                s = (f[q] - f[r] + q * q - r * r) / (q - r) / 2;
            } while (s <= z[k] && --k >= 0);

            k++;
            v[k] = q;
            z[k] = s;
            z[k + 1] = DF_INF;
        }

        k = 0;
        for (int32_t q = 0; q < n; q++) {
            while (z[k + 1] < q) k++;
            int32_t r = v[k];
            int32_t d = q - r;
            Df[q * (int32_t)stride] = f[r] + d * d;
        }
    }
};

// TODO: pseudo-infinite map?

};