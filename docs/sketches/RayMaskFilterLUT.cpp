// Generate ray optimization masks
// Based on https://www.youtube.com/watch?v=P2bGF6GPmfc

#include <cstdint>

const int32_t kOctDirQuantScale = 10;
const int32_t kNumDirs = kOctDirQuantScale * kOctDirQuantScale;
const int32_t kTableSize = kNumDirs * 64;

extern const uint64_t g_RayMaskFilterLUT[kTableSize];

#if 1
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

using namespace glm;

static vec3 GetSideDist(vec3 pos, vec3 dir) {
    pos = fract(pos);
    return mix(1.0f - pos, pos, lessThan(dir, vec3(0.0)));
    // return dir < 0.0 ? pos : 1.0 - pos;
}

static uint64_t GenerateRayMaskDDA(vec3 origin, vec3 dir) {
    uint64_t mask = 0;

    vec3 deltaDist = abs(1.0f / dir);
    vec3 sideDist = GetSideDist(origin, dir) * deltaDist;
    ivec3 pos = floor(origin);

    for (uint64_t i = 0; i < 12; i++) {
        if (uint32_t(pos.x | pos.y | pos.z) >= 4) break;
        mask |= 1ull << (pos.x + pos.z * 4 + pos.y * 16);

        if (sideDist.x < sideDist.y && sideDist.x < sideDist.z) {
            sideDist.x += deltaDist.x;
            pos.x += dir.x < 0 ? -1 : +1;
        } else if (sideDist.y < sideDist.z) {
            sideDist.y += deltaDist.y;
            pos.y += dir.y < 0 ? -1 : +1;
        } else {
            sideDist.z += deltaDist.z;
            pos.z += dir.z < 0 ? -1 : +1;
        }
    }
    return mask;
}

static vec3 MartinR2(uint32_t index) {
    const vec3 a = vec3(0.8191725133961644, 0.671043606703789, 0.5497004779019701);
    return fract(a * float(index) + 0.5f);
}

// https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/
// https://johnwhite3d.blogspot.com/2017/10/signed-octahedron-normal-encoding.html
static vec2 OctWrap(vec2 v) {
    // return ( 1.0 - abs( v.yx ) ) * ( v.xy >= 0.0 ? 1.0 : -1.0 );
    vec2 tmp = 1.0f - abs(vec2(v.y, v.x));
    return mix(-tmp, tmp, greaterThanEqual(v, vec2(0.0)));
}
static vec2 OctEncodeNormal(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));

    vec2 e = vec2(n);
    if (n.z >= 0.0) e = OctWrap(e);
    return e * 0.5f + 0.5f;
}
static vec3 OctDecodeNormal(vec2 encN) {
    encN = encN * 2.0f - 1.0f;

    vec3 n;
    n.z = 1.0 - abs(encN.x) - abs(encN.y);
    vec2 d = n.z >= 0.0 ? encN : OctWrap(encN);
    n.x = d.x;
    n.y = d.y;
    return normalize(n);
}

static uint32_t GetDirectionIndex(vec3 dir) {
    vec2 enc = OctEncodeNormal(dir);
    uint32_t idx = round(enc.x * kOctDirQuantScale) + round(enc.y * kOctDirQuantScale) * kOctDirQuantScale;
    return idx;
}
vec3 GetDirectionFromIndex(uint32_t idx) {
    vec2 enc = vec2(idx % kOctDirQuantScale, idx / kOctDirQuantScale) + 0.5f;
    return OctDecodeNormal(enc * (1.0f / kOctDirQuantScale));
}

static vec3 SampleDirection(vec2 sample) {
    float y = sample.y * 2.0f - 1.0f;
    float sy = sqrt(1.0f - y * y);

    float x = sin(sample.x * tau<float>());
    float z = cos(sample.x * tau<float>());

    return { x * sy, y, z * sy };
}

static uint64_t GenerateRayFilterMask(ivec3 pos, vec3 centerDir) {
    uint64_t mask = 0;
    uint32_t centerDirIdx = GetDirectionIndex(centerDir);

    for (uint32_t k1 = 0; k1 < 256; k1++) {
        for (uint32_t k2 = 0; k2 < 256; k2++) {
            vec3 dir = SampleDirection(vec2(k1, k2) / 255.0f);
            if (GetDirectionIndex(dir) != centerDirIdx) continue;

            for (uint32_t j = 0; j < 64; j++) {
                vec3 origin = vec3(pos) + MartinR2(j);
                mask |= GenerateRayMaskDDA(origin, dir);
            }
        }
    }
    return mask;
}

#include <format>
#include <iostream>

int main() {
    for (uint64_t dirIdx = 0; dirIdx < kNumDirs; dirIdx++) {
        vec3 dir = GetDirectionFromIndex(dirIdx);
        std::cout << std::format("\n// Direction #{} ({:.3f} {:.3f} {:.3f})", dirIdx, dir.x, dir.y, dir.z);

        for (uint64_t originIdx = 0; originIdx < 64; originIdx++) {
            if (originIdx % 8 == 0) std::cout << std::endl;
            // table[originIdx + dirIdx * 64]
            ivec3 pos = ivec3(originIdx) >> ivec3(0, 4, 2) & 3;
            uint64_t mask = GenerateRayFilterMask(pos, dir);

            std::cout << std::format("0x{:016x}, ", mask);
        }
    }
    return 0;
}
#endif

// clang-format off
const uint64_t g_RayMaskFilterLUT[] = {
// TODO
};