#pragma once

#include <immintrin.h>

#include <cassert>
#include <cstdint>
#include <bit>
#include <glm/mat4x4.hpp>

struct VInt {
    static const uint32_t Length = sizeof(__m512i) / sizeof(int32_t);

    __m512i reg;

    VInt() { reg = _mm512_setzero_epi32(); }
    VInt(__m512i x) { reg = x; }
    VInt(int32_t x) { reg = _mm512_set1_epi32(x); }
    inline operator __m512i() const { return reg; }

    inline int32_t& operator[](size_t idx) const {
        assert(idx >= 0 && idx < Length);
        return ((int32_t*)&reg)[idx];
    }

    static inline VInt load(const void* ptr) { return _mm512_loadu_si512((__m512i*)ptr); }
    inline void store(void* ptr) const { _mm512_storeu_si512((__m512i*)ptr, reg); }

    template<int IndexScale = 1>
    static inline VInt gather(const void* basePtr, VInt indices) {
        return _mm512_i32gather_epi32(indices, basePtr, IndexScale);
    }

    static inline VInt ramp() { return _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
};
struct VFloat {
    static const uint32_t Length = sizeof(__m512) / sizeof(float);

    __m512 reg;

    VFloat() { reg = _mm512_setzero_ps(); }
    VFloat(__m512 x) { reg = x; }
    VFloat(float x) { reg = _mm512_set1_ps(x); }
    inline operator __m512() const { return reg; }

    inline float& operator[](size_t idx) const {
        assert(idx >= 0 && idx < Length);
        return ((float*)&reg)[idx];
    }

    static inline VFloat load(const void* ptr) { return _mm512_loadu_ps(ptr); }
    inline void store(void* ptr) const { _mm512_storeu_ps(ptr, reg); }

    template<int IndexScale = 1>
    static inline VFloat gather(const void* basePtr, VInt indices) {
        return _mm512_i32gather_ps(indices.reg, basePtr, IndexScale);
    }

    static inline VFloat ramp() { return _mm512_setr_ps(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15); }
};
using VMask = uint16_t;

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

inline VFloat3::VFloat3(const VFloat4& v) { x = v.x, y = v.y, z = v.z; }

#define _SIMD_DEF_OPERATORS(V, OpSuffix, MulOp, BitSuffix)                                          \
    inline V operator+(V a, V b) { return _mm512_add_##OpSuffix(a, b); }                            \
    inline V operator-(V a, V b) { return _mm512_sub_##OpSuffix(a, b); }                            \
    inline V operator*(V a, V b) { return _mm512_##MulOp(a, b); }                                   \
    inline V operator&(V a, V b) { return _mm512_and_##BitSuffix(a, b); }                           \
    inline V operator|(V a, V b) { return _mm512_or_##BitSuffix(a, b); }                            \
    inline V operator^(V a, V b) { return _mm512_xor_##BitSuffix(a, b); }                           \
                                                                                                    \
    inline V operator+=(V& a, V b) { return a = (a + b); }                                          \
    inline V operator-=(V& a, V b) { return a = (a - b); }                                          \
    inline V operator*=(V& a, V b) { return a = (a * b); }                                          \
                                                                                                    \
    inline VMask operator<(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_LT); }  \
    inline VMask operator>(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_GT); }  \
    inline VMask operator<=(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_LE); } \
    inline VMask operator>=(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_GE); } \
    inline VMask operator==(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_EQ); } \
    inline VMask operator!=(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_NE); }

_SIMD_DEF_OPERATORS(VFloat, ps, mul_ps, ps);
inline VFloat operator/(VFloat a, VFloat b) { return _mm512_div_ps(a, b); }
inline VFloat operator-(VFloat a) { return a ^ -0.0f; }

_SIMD_DEF_OPERATORS(VInt, epi32, mullo_epi32, si512);
inline VInt operator>>(VInt a, uint32_t b) { return _mm512_srai_epi32(a, b); }
inline VInt operator<<(VInt a, uint32_t b) { return _mm512_slli_epi32(a, b); }

inline VInt operator>>(VInt a, VInt b) { return _mm512_srav_epi32(a, b); }
inline VInt operator<<(VInt a, VInt b) { return _mm512_sllv_epi32(a, b); }
inline VInt operator~(VInt a) { return a ^ ~0; }
inline VInt operator|=(VInt& a, VInt b) { return a = (a | b); }

inline VFloat2 operator+(VFloat2 a, VFloat2 b) { return { a.x + b.x, a.y + b.y }; }
inline VFloat2 operator-(VFloat2 a, VFloat2 b) { return { a.x - b.x, a.y - b.y }; }
inline VFloat2 operator*(VFloat2 a, VFloat2 b) { return { a.x * b.x, a.y * b.y }; }
inline VFloat2 operator/(VFloat2 a, VFloat2 b) { return { a.x / b.x, a.y / b.y }; }

inline VFloat3 operator+(VFloat3 a, VFloat3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline VFloat3 operator-(VFloat3 a, VFloat3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline VFloat3 operator*(VFloat3 a, VFloat3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline VFloat3 operator/(VFloat3 a, VFloat3 b) { return { a.x / b.x, a.y / b.y, a.z / b.z }; }
inline VFloat3 operator+=(VFloat3& a, VFloat3 b) { return a = (a + b); }
inline VFloat3 operator*=(VFloat3& a, VFloat3 b) { return a = (a * b); }

inline VFloat4 operator+(VFloat4 a, VFloat4 b) { return { a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
inline VFloat4 operator-(VFloat4 a, VFloat4 b) { return { a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }
inline VFloat4 operator*(VFloat4 a, VFloat4 b) { return { a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w }; }
inline VFloat4 operator/(VFloat4 a, VFloat4 b) { return { a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w }; }

// Math ops
namespace simd {
// Pixel offsets within a 4x4 tile/fragment
//   X: [0,1,2,3, 0,1,2,3, ...]
//   Y: [0,0,0,0, 1,1,1,1, ...]
inline const VInt FragPixelOffsetsX = VInt::ramp() & 3, FragPixelOffsetsY = VInt::ramp() >> 2;

inline VInt round2i(VFloat x) { return _mm512_cvtps_epi32(x.reg); }
inline VInt trunc2i(VFloat x) { return _mm512_cvttps_epi32(x.reg); }
inline VInt floor2i(VFloat x) { return _mm512_cvt_roundps_epi32(x.reg, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); }
inline VFloat conv2f(VInt x) { return _mm512_cvtepi32_ps(x.reg); }

inline VFloat floor(VFloat x) { return _mm512_floor_ps(x); }
inline VFloat fract(VFloat x) { return _mm512_reduce_ps(x, _MM_FROUND_TO_NEG_INF); }

inline VInt re2i(VFloat x) { return _mm512_castps_si512(x); }  // reinterpret float bits to int
inline VFloat re2f(VInt x) { return _mm512_castsi512_ps(x); }  // reinterpret int to float bits

inline VInt min(VInt x, VInt y) { return _mm512_min_epi32(x, y); }
inline VInt max(VInt x, VInt y) { return _mm512_max_epi32(x, y); }

inline VFloat min(VFloat x, VFloat y) { return _mm512_min_ps(x, y); }
inline VFloat max(VFloat x, VFloat y) { return _mm512_max_ps(x, y); }

//x * y + z
inline VFloat fma(VFloat x, VFloat y, VFloat z) { return _mm512_fmadd_ps(x, y, z); }
//x * y - z
inline VFloat fms(VFloat x, VFloat y, VFloat z) { return _mm512_fmsub_ps(x, y, z); }

// Linear interpolation between `a` and `b`: `a*(1-t) + b*t`
// https://fgiesen.wordpress.com/2012/08/15/linear-interpolation-past-present-and-future/
inline VFloat lerp(VFloat a, VFloat b, VFloat t) { return _mm512_fmadd_ps(t, b, _mm512_fnmadd_ps(t, a, a)); }

inline VFloat sqrt(VFloat x) { return _mm512_sqrt_ps(x); }
// approximate sqrt (14-bits precision)
inline VFloat sqrt14(VFloat x) { return _mm512_mul_ps(_mm512_rsqrt14_ps(x), x); }
// approximate reciprocal sqrt (14-bits precision)
inline VFloat rsqrt14(VFloat x) { return _mm512_rsqrt14_ps(x); }
// approximate reciprocal (14-bits precision)
inline VFloat rcp14(VFloat x) { return _mm512_rcp14_ps(x); }

inline VFloat abs(VFloat x) { return _mm512_abs_ps(x); }
inline VInt abs(VInt x) { return _mm512_abs_epi32(x); }

// lanewise: cond ? a : b
inline VFloat csel(VMask cond, VFloat a, VFloat b) { return _mm512_mask_mov_ps(b, cond, a); }
inline VInt csel(VMask cond, VInt a, VInt b) { return _mm512_mask_mov_epi32(b, cond, a); }

// if (cond) dest = x
inline void set_if(VMask cond, VFloat& dest, VFloat x) { dest = csel(cond, x, dest); }
inline void set_if(VMask cond, VInt& dest, VInt x) { dest = csel(cond, x, dest); }

inline bool any(VMask cond) { return cond != 0; }
inline bool all(VMask cond) { return cond == 0xFFFF; }

// 16-bit linear interpolation with 15-bit interpolant: a + (b - a) * t
// mulhrs(a, b) = (a * b + (1 << 14)) >> 15
inline VInt lerp16(VInt a, VInt b, VInt t) { return _mm512_add_epi16(a, _mm512_mulhrs_epi16(_mm512_sub_epi16(b, a), t)); }

// shift right logical
inline VInt shrl(VInt a, uint32_t b) { return _mm512_srli_epi32(a, b); }
inline VInt shrl(VInt a, VInt b) { return _mm512_srlv_epi32(a, b); }

// Reverse bits of packed 32-bit integers.
// https://wunkolo.github.io/post/2020/11/gf2p8affineqb-bit-reversal/
inline VInt bitrev(VInt x) {
    const auto A = _mm512_set1_epi64(0b10000000'01000000'00100000'00010000'00001000'00000100'00000010'00000001LL);
    const auto B = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

    auto rev8 = _mm512_gf2p8affine_epi64_epi8(x, A, 0);
    return _mm512_shuffle_epi8(rev8, _mm512_broadcast_i32x4(B));
}
inline VInt lzcnt(VInt x) { return _mm512_lzcnt_epi32(x); }

// https://en.wikipedia.org/wiki/Find_first_set#Properties_and_relations
// tzcnt(x) = 31 - lzcnt(x & -x)
//          = popcnt((x & -x) - 1)   (popcnt 3c vs lzcnt 4c, avoids const load from mem -> reduced latency)
inline VInt tzcnt(VInt x) { return _mm512_popcnt_epi32((x & (0 - x)) - 1); }

inline VFloat dot(VFloat3 a, VFloat3 b) {
    return fma(a.x, b.x, fma(a.y, b.y, a.z * b.z));
}
inline VFloat3 normalize(VFloat3 a) {
    VFloat len = _mm512_rsqrt14_ps(dot(a, a));
    return { a.x * len, a.y * len, a.z * len };
}
inline VFloat3 cross(VFloat3 a, VFloat3 b) {
    return {
        fms(a.y, b.z, a.z * b.y),
        fms(a.z, b.x, a.x * b.z),
        fms(a.x, b.y, a.y * b.x),
    };
}
inline VFloat3 reflect(VFloat3 i, VFloat3 n) { return i - 2.0f * dot(n, i) * n; }

inline const float pi = 3.141592653589793f;
inline const float tau = 6.283185307179586f;
inline const float inv_pi = 0.3183098861837907f;

// Sleef xfastsinf_u3500()
inline VFloat sin(VFloat a) {
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
inline VFloat cos(VFloat a) {
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
inline void sincos(VFloat a, VFloat& rs, VFloat& rc) {
    VInt q = round2i(a * inv_pi);
    VFloat d = fma(conv2f(q), -pi, a);

    VFloat s = d * d;
    VFloat u = -0.1881748176e-3f;
    u = fma(u, s, +0.8323502727e-2f);
    u = fma(u, s, -0.1666651368e+0f);
    u = fma(s * d, u, d);

    VFloat qs = re2f(q << 31);
    rs = u ^ qs;  // if ((q & 1) != 0) u = -u;
    rc = sqrt14(1.0f - rs * rs) ^ qs;
}

// Approximates sin(2πx) and cos(2πx)
// Max relative error: sin=0.00721228 cos=0.000597186
// https://publik-void.github.io/sin-cos-approximations
inline void sincos_2pi(VFloat x, VFloat& s, VFloat& c) {
    // Reduce range to -1/4..1/4
    //   x = x + 0.25
    //   x = abs(x - floor(x + 0.5)) - 0.25
    VFloat xr = _mm512_reduce_ps(x + 0.25f, _MM_FROUND_TO_NEAREST_INT);
    VFloat x1 = abs(xr) - 0.25f;
    VFloat x2 = x1 * x1;

    s = x1 * fma(x2, -36.26749369f, 6.23786927f);
    c = fma(x2, fma(x2, 57.34151006f, -19.56474772f), 0.99940322f) | (xr & -0.0f);
}

// https://github.com/romeric/fastapprox/blob/master/fastapprox/src/fastlog.h
inline VFloat approx_log2(VFloat x) {
    VFloat y = conv2f(re2i(x));
    return fma(y, 1.1920928955078125e-7f, -126.94269504f);
}
inline VFloat approx_exp2(VFloat x) {
    x = max(x, -126.0f);
    return re2f(round2i((1 << 23) * (x + 126.94269504f)));
}
inline VFloat approx_pow(VFloat x, VFloat y) { return approx_exp2(approx_log2(x) * y); }

inline VInt ilog2(VFloat x) {
    return (re2i(x) >> 23) - 127;  // log(x) for x <= 0 is undef, so no need to mask sign out
}

// Calculate coarse partial derivatives for a 4x4 fragment.
// https://gamedev.stackexchange.com/a/130933
inline VFloat dFdx(VFloat p) {
    auto a = _mm512_shuffle_ps(p, p, 0b10'10'00'00);  //[0 0 2 2]
    auto b = _mm512_shuffle_ps(p, p, 0b11'11'01'01);  //[1 1 3 3]
    return _mm512_sub_ps(b, a);
}
inline VFloat dFdy(VFloat p) {
    // auto a = _mm256_permute2x128_si256(p, p, 0b00'00'00'00);  // dupe lower 128 lanes
    // auto b = _mm256_permute2x128_si256(p, p, 0b01'01'01'01);  // dupe upper 128 lanes
    auto a = _mm512_shuffle_f32x4(p, p, 0b10'10'00'00);
    auto b = _mm512_shuffle_f32x4(p, p, 0b11'11'01'01);
    return _mm512_sub_ps(b, a);
}

inline VFloat4 TransformVector(const glm::mat4& m, const VFloat4& v) {
    return {
        m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
        m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
        m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
        m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w,
    };
}
inline VFloat3 TransformNormal(const glm::mat4& m, const VFloat3& n) {
    return {
        m[0][0] * n.x + m[1][0] * n.y + m[2][0] * n.z,
        m[0][1] * n.x + m[1][1] * n.y + m[2][1] * n.z,
        m[0][2] * n.x + m[1][2] * n.y + m[2][2] * n.z,
    };
}
inline VFloat4 PerspectiveDiv(const VFloat4& v) {
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

class BitIter {
    uint32_t _mask;

public:
    BitIter(uint32_t mask) : _mask(mask) {}

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
        VFloat sy = simd::sqrt14(1.0f - y * y);

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

        s[0] = _mm512_rol_epi32(s0, 26) ^ s1 ^ (s1 << 9);  // a, b
        s[1] = _mm512_rol_epi32(s1, 13);                   // c

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