#pragma once

#include <immintrin.h>

#include <cassert>
#include <cstdint>
#include <bit>
#include <glm/mat4x4.hpp>

#define SIMD_INLINE [[gnu::always_inline]] inline

#if __AVX512F__
    #define SIMD_AVX512 1
    #include "SIMD_AVX512.h"
#elif __AVX2__
    #define SIMD_AVX2 1
    #include "SIMD_AVX2.h"
#else
    #error "Platform or compiler not supported"
#endif

struct VFloat4;

struct VFloat2 {
    VFloat x, y;

    VFloat2() = default;
    VFloat2(float v) { x = y = v; }
    VFloat2(VFloat v) { x = y = v; }
    VFloat2(VFloat x_, VFloat y_) { x = x_, y = y_; }
    VFloat2(const glm::vec2& v) { x = v.x, y = v.y; }
};
struct VFloat3 {
    VFloat x, y, z;

    VFloat3() = default;
    VFloat3(float v) { x = y = z = v; }
    VFloat3(VFloat v) { x = y = z = v; }
    VFloat3(VFloat x_, VFloat y_, VFloat z_) { x = x_, y = y_, z = z_; }
    VFloat3(const glm::vec3& v) { x = v.x, y = v.y, z = v.z; }
    explicit VFloat3(const VFloat4& v);
};
struct VFloat4 {
    VFloat x, y, z, w;

    VFloat4() = default;
    VFloat4(float v) { x = y = z = w = v; }
    VFloat4(VFloat v) { x = y = z = w = v; }
    VFloat4(VFloat x_, VFloat y_, VFloat z_, VFloat w_) { x = x_, y = y_, z = z_, w = w_; }
    VFloat4(VFloat3 a, VFloat w_) { x = a.x, y = a.y, z = a.z, w = w_; }
    VFloat4(const glm::vec4& v) { x = v.x, y = v.y, z = v.z, w = v.w; }
};

SIMD_INLINE VFloat2 operator+(VFloat2 a, VFloat2 b) { return { a.x + b.x, a.y + b.y }; }
SIMD_INLINE VFloat2 operator-(VFloat2 a, VFloat2 b) { return { a.x - b.x, a.y - b.y }; }
SIMD_INLINE VFloat2 operator*(VFloat2 a, VFloat2 b) { return { a.x * b.x, a.y * b.y }; }
SIMD_INLINE VFloat2 operator/(VFloat2 a, VFloat2 b) { return { a.x / b.x, a.y / b.y }; }

SIMD_INLINE VFloat3 operator+(VFloat3 a, VFloat3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
SIMD_INLINE VFloat3 operator-(VFloat3 a, VFloat3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
SIMD_INLINE VFloat3 operator*(VFloat3 a, VFloat3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
SIMD_INLINE VFloat3 operator/(VFloat3 a, VFloat3 b) { return { a.x / b.x, a.y / b.y, a.z / b.z }; }
SIMD_INLINE VFloat3 operator+=(VFloat3& a, VFloat3 b) { return a = (a + b); }
SIMD_INLINE VFloat3 operator*=(VFloat3& a, VFloat3 b) { return a = (a * b); }

SIMD_INLINE VFloat3::VFloat3(const VFloat4& v) { x = v.x, y = v.y, z = v.z; }

SIMD_INLINE VFloat4 operator+(VFloat4 a, VFloat4 b) { return { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
SIMD_INLINE VFloat4 operator-(VFloat4 a, VFloat4 b) { return { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }
SIMD_INLINE VFloat4 operator*(VFloat4 a, VFloat4 b) { return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w }; }
SIMD_INLINE VFloat4 operator/(VFloat4 a, VFloat4 b) { return { a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w }; }

// Common math ops
namespace simd {
// Pixel offsets within a SIMD tile
//   X: [0,1,2,3, 0,1,2,3, ...]
//   Y: [0,0,0,0, 1,1,1,1, ...]
inline const VInt FragPixelOffsetsX = RampI & 3, FragPixelOffsetsY = RampI >> 2;

inline const float pi = 3.141592653589793f;
inline const float tau = 6.283185307179586f;
inline const float inv_pi = 0.3183098861837907f;

SIMD_INLINE VFloat dot(VFloat3 a, VFloat3 b) {
    return fma(a.x, b.x, fma(a.y, b.y, a.z * b.z));
}
SIMD_INLINE VFloat3 normalize(VFloat3 a) {
    VFloat len = approx_rsqrt(dot(a, a));
    return { a.x * len, a.y * len, a.z * len };
}
SIMD_INLINE VFloat3 cross(VFloat3 a, VFloat3 b) {
    return {
        fms(a.y, b.z, a.z * b.y),
        fms(a.z, b.x, a.x * b.z),
        fms(a.x, b.y, a.y * b.x),
    };
}
SIMD_INLINE VFloat3 reflect(VFloat3 i, VFloat3 n) { return i - 2.0f * dot(n, i) * n; }

// Sleef xfastsinf_u3500()
SIMD_INLINE VFloat sin(VFloat a) {
    VInt q = round2i(a * inv_pi);
    VFloat d = fma(conv2f(q), -pi, a);

    VFloat s = d * d;
    VFloat u = -0.1881748176e-3f;
    u = fma(u, s, +0.8323502727e-2f);
    u = fma(u, s, -0.1666651368e+0f);
    u = fma(s * d, u, d);
    u = u ^ re2f(q << 31);  // if ((q & 1) != 0) u = -u;
    return u;
}

// Sleef xfastcosf_u3500()
SIMD_INLINE VFloat cos(VFloat a) {
    VInt q = round2i(fma(a, inv_pi, -0.5f));
    VFloat d = fma(conv2f(q), -pi, a - (pi * 0.5f));

    VFloat s = d * d;
    VFloat u = -0.1881748176e-3f;
    u = fma(u, s, +0.8323502727e-2f);
    u = fma(u, s, -0.1666651368e+0f);
    u = fma(s * d, u, d);
    u = u ^ re2f((~0 ^ q) << 31);  // if ((q & 1) == 0) u = -u;
    return u;
}

// Max relative error: sin=3.45707e-06 cos=0.00262925
SIMD_INLINE void sincos(VFloat a, VFloat& rs, VFloat& rc) {
    VInt q = round2i(a * inv_pi);
    VFloat d = fma(conv2f(q), -pi, a);

    VFloat s = d * d;
    VFloat u = -0.1881748176e-3f;
    u = fma(u, s, +0.8323502727e-2f);
    u = fma(u, s, -0.1666651368e+0f);
    u = fma(s * d, u, d);

    VFloat qs = re2f(q << 31);
    rs = u ^ qs;  // if ((q & 1) != 0) u = -u;
    rc = approx_rsqrt(1.0f - rs * rs) ^ qs;
}

// Approximates sin(2πx) and cos(2πx)
// Max relative error: sin=0.00721228 cos=0.000597186
// https://publik-void.github.io/sin-cos-approximations
SIMD_INLINE void sincos_2pi(VFloat x, VFloat& s, VFloat& c) {
    // Reduce range to -1/4..1/4
    //   x = x + 0.25
    //   x = abs(x - floor(x + 0.5)) - 0.25
#if _SIMD_AVX512
    VFloat xr = _mm512_reduce_ps(x + 0.25f, _MM_FROUND_TO_NEAREST_INT);
#else
    x = x + 0.25f;
    VFloat xr = x - round(x);
#endif
    VFloat x1 = abs(xr) - 0.25f;
    VFloat x2 = x1 * x1;

    s = x1 * fma(x2, -36.26749369f, 6.23786927f);
    c = fma(x2, fma(x2, 57.34151006f, -19.56474772f), 0.99940322f) | (xr & -0.0f);
}

// https://github.com/romeric/fastapprox/blob/master/fastapprox/src/fastlog.h
SIMD_INLINE VFloat approx_log2(VFloat x) {
    VFloat y = conv2f(re2i(x));
    return fma(y, 1.1920928955078125e-7f, -126.94269504f);
}
SIMD_INLINE VFloat approx_exp2(VFloat x) {
    x = max(x, -126.0f);
    return re2f(round2i((1 << 23) * (x + 126.94269504f)));
}
SIMD_INLINE VFloat approx_pow(VFloat x, VFloat y) { return approx_exp2(approx_log2(x) * y); }

SIMD_INLINE VInt ilog2(VFloat x) {
    return (re2i(x) >> 23) - 127;  // log(x) for x <= 0 is undef, so no need to mask sign out
}

SIMD_INLINE VFloat4 TransformVector(const glm::mat4& m, const VFloat4& v) {
    return {
        fma(m[0][0], v.x, fma(m[1][0], v.y, fma(m[2][0], v.z, m[3][0] * v.w))),
        fma(m[0][1], v.x, fma(m[1][1], v.y, fma(m[2][1], v.z, m[3][1] * v.w))),
        fma(m[0][2], v.x, fma(m[1][2], v.y, fma(m[2][2], v.z, m[3][2] * v.w))),
        fma(m[0][3], v.x, fma(m[1][3], v.y, fma(m[2][3], v.z, m[3][3] * v.w)))
    };
}
SIMD_INLINE VFloat3 TransformNormal(const glm::mat4& m, const VFloat3& n) {
    return {
        fma(m[0][0], n.x, fma(m[1][0], n.y, m[2][0] * n.z)),
        fma(m[0][1], n.x, fma(m[1][1], n.y, m[2][1] * n.z)),
        fma(m[0][2], n.x, fma(m[1][2], n.y, m[2][2] * n.z)),
    };
}
SIMD_INLINE VFloat4 PerspectiveDiv(const VFloat4& v) {
    VFloat rw = 1.0f / v.w;
    return { v.x * rw, v.y * rw, v.z * rw, rw };
}

};  // namespace simd

template<typename T>
struct DeleteAligned {
    void operator()(T* data) const { _mm_free(data); }
};

template<typename T>
using AlignedBuffer = std::unique_ptr<T[], DeleteAligned<T>>;

template<typename T>
AlignedBuffer<T> alloc_buffer(size_t count, size_t align = 64) {
    T* ptr = (T*)_mm_malloc(count * sizeof(T), align);
    return AlignedBuffer<T>(ptr);
}

template<typename T = uint32_t>
class BitIter {
    T _mask;

public:
    BitIter(T mask) : _mask(mask) {}
    
    BitIter(__m256i mask) : _mask(_mm256_movemask_ps(_mm256_castps_si256(mask))) {}
    BitIter(__m256 mask) : _mask(_mm256_movemask_ps(mask)) {}

    BitIter& operator++() {
        _mask &= (_mask - 1);
        return *this;
    }
    uint32_t operator*() const { return (uint32_t)std::countr_zero(_mask); }
    friend bool operator!=(const BitIter& a, const BitIter& b) { return a._mask != b._mask; }

    BitIter begin() const { return *this; }
    BitIter end() const { return BitIter(0); }
};

// Parallel PRNG
// https://prng.di.unimi.it/xoroshiro64star.c
struct VRandom {
    VRandom(uint64_t seed) {
        for (uint32_t i = 0; i < sizeof(s); i += 8) {
            uint64_t state = SplitMix64(seed);
            memcpy((uint8_t*)&s + i, &state, 8);
        }
    }

    // Returns a vector of random uniformly distributed floats in range [0..1)
    VFloat NextUnsignedFloat() {
        VFloat frac = simd::re2f(simd::shrl(NextU32(), 9));
        return (1.0f | frac) - 1.0f;
    }

    // Returns a vector of random uniformly distributed floats in range [-1..1)
    VFloat NextSignedFloat() {
        VFloat frac = simd::re2f(simd::shrl(NextU32(), 9));
        return (2.0f | frac) - 3.0f;
    }

    // Returns a random spherical direction
    VFloat3 NextDirection() {
        // azimuth = rand() * PI * 2
        // y = rand() * 2 - 1
        // sin_elevation = sqrt(1 - y * y)
        // x = sin_elevation * cos(azimuth);
        // z = sin_elevation * sin(azimuth);
        VInt rand = NextU32();

        const float randScale = (1.0f / (1 << 15));
        VFloat y = simd::conv2f(rand >> 16) * randScale;  // signed
        VFloat a = simd::conv2f(rand & 0x7FFF) * randScale;

        VFloat x, z;
        simd::sincos_2pi(a, x, z);
        VFloat sy = simd::approx_sqrt(1.0f - y * y);

        return { x * sy, y, z * sy };
    }

    VFloat3 NextHemisphereDirection(const VFloat3& normal) {
        VFloat3 dir = NextDirection();
        VFloat sign = simd::dot(dir, normal) & -0.0f;
        return { dir.x ^ sign, dir.y ^ sign, dir.z ^ sign };
    }

    VInt NextU32() {
        VInt s0 = s[0];
        VInt s1 = s[1] ^ s0;
        // VInt result = s0 * (int32_t)0x9E3779BB;

        s[0] = simd::rotl(s0, 26) ^ s1 ^ (s1 << 9);  // a, b
        s[1] = simd::rotl(s1, 13);                   // c

        return s0;
    }

private:
    VInt s[2];

    static uint64_t SplitMix64(uint64_t& x) {
        uint64_t z = (x += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }
};