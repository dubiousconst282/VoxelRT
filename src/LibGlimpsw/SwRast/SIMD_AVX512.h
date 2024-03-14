#pragma once

using VMask = uint16_t;

struct VInt {
    static const uint32_t Length = sizeof(__m512i) / sizeof(int32_t);

    __m512i reg;

    SIMD_INLINE VInt() { reg = _mm512_set1_epi32(0); }
    SIMD_INLINE VInt(__m512i x) { reg = x; }
    SIMD_INLINE VInt(int32_t x) { reg = _mm512_set1_epi32(x); }
    SIMD_INLINE operator __m512i() const { return reg; }

    SIMD_INLINE int32_t& operator[](size_t idx) const {
        assert(idx < Length);
        return ((int32_t*)&reg)[idx];
    }

    SIMD_INLINE static VInt load(const void* ptr) { return _mm512_loadu_epi32(ptr); }
    SIMD_INLINE void store(void* ptr) const { _mm512_storeu_epi32(ptr, reg); }

    SIMD_INLINE static VInt mask_load(const void* ptr, VMask mask) { return _mm512_mask_loadu_epi32(_mm512_set1_epi32(0), mask, ptr); }
    SIMD_INLINE void mask_store(void* ptr, VMask mask) { _mm512_mask_storeu_epi32(ptr, mask, reg); }

    template<int IndexScale = 4>
    SIMD_INLINE static VInt gather(const void* basePtr, VInt indices) {
        return _mm512_i32gather_epi32(indices.reg, basePtr, IndexScale);
    }
    template<int IndexScale = 4>
    SIMD_INLINE static VInt mask_gather(const void* basePtr, VInt indices, VMask mask) {
        return _mm512_mask_i32gather_epi32(_mm512_set1_epi32(0), mask, indices.reg, basePtr, IndexScale);
    }

    SIMD_INLINE static VInt shuffle(VInt table, VInt index) { return _mm512_permutexvar_epi32(index, table); }
};
struct VFloat {
    static const uint32_t Length = sizeof(__m512) / sizeof(float);

    __m512 reg;

    SIMD_INLINE VFloat() { reg = _mm512_set1_ps(0.0f); }
    SIMD_INLINE VFloat(__m512 x) { reg = x; }
    SIMD_INLINE VFloat(float x) { reg = _mm512_set1_ps(x); }
    SIMD_INLINE operator __m512() const { return reg; }

    SIMD_INLINE float& operator[](size_t idx) const {
        assert(idx < Length);
        return ((float*)&reg)[idx];
    }

    SIMD_INLINE static VFloat load(const void* ptr) { return _mm512_loadu_ps(ptr); }
    SIMD_INLINE void store(void* ptr) const { _mm512_storeu_ps(ptr, reg); }

    SIMD_INLINE static VFloat mask_load(const void* ptr, VMask mask) { return _mm512_mask_loadu_ps(_mm512_set1_ps(0.0f), mask, ptr); }
    SIMD_INLINE void mask_store(void* ptr, VMask mask) { _mm512_mask_storeu_ps(ptr, mask, reg); }

    template<int IndexScale = 4>
    SIMD_INLINE static VFloat gather(const void* basePtr, VInt indices) {
        return _mm512_i32gather_ps(indices.reg, basePtr, IndexScale);
    }
    template<int IndexScale = 4>
    SIMD_INLINE static VFloat mask_gather(const void* basePtr, VInt indices, VMask mask) {
        return _mm512_mask_i32gather_ps(_mm512_set1_ps(0.0f), mask, indices.reg, basePtr, IndexScale);
    }
};

#define _SIMD_DEF_OPERATORS(V, OpSuffix, MulOp, BitSuffix)                                               \
    SIMD_INLINE V operator+(V a, V b) { return _mm512_add_##OpSuffix(a, b); }                            \
    SIMD_INLINE V operator-(V a, V b) { return _mm512_sub_##OpSuffix(a, b); }                            \
    SIMD_INLINE V operator*(V a, V b) { return _mm512_##MulOp(a, b); }                                   \
    SIMD_INLINE V operator&(V a, V b) { return _mm512_and_##BitSuffix(a, b); }                           \
    SIMD_INLINE V operator|(V a, V b) { return _mm512_or_##BitSuffix(a, b); }                            \
    SIMD_INLINE V operator^(V a, V b) { return _mm512_xor_##BitSuffix(a, b); }                           \
                                                                                                         \
    SIMD_INLINE V operator+=(V& a, V b) { return a = (a + b); }                                          \
    SIMD_INLINE V operator-=(V& a, V b) { return a = (a - b); }                                          \
    SIMD_INLINE V operator*=(V& a, V b) { return a = (a * b); }                                          \
                                                                                                         \
    SIMD_INLINE VMask operator<(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_LT); }  \
    SIMD_INLINE VMask operator>(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_GT); }  \
    SIMD_INLINE VMask operator<=(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_LE); } \
    SIMD_INLINE VMask operator>=(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_GE); } \
    SIMD_INLINE VMask operator==(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_EQ); } \
    SIMD_INLINE VMask operator!=(V a, V b) { return _mm512_cmp_##OpSuffix##_mask(a, b, _MM_CMPINT_NE); }

_SIMD_DEF_OPERATORS(VFloat, ps, mul_ps, ps);
SIMD_INLINE VFloat operator/(VFloat a, VFloat b) { return _mm512_div_ps(a, b); }
SIMD_INLINE VFloat operator-(VFloat a) { return a ^ -0.0f; }

_SIMD_DEF_OPERATORS(VInt, epi32, mullo_epi32, si512);
SIMD_INLINE VInt operator>>(VInt a, uint32_t b) { return _mm512_srai_epi32(a, b); }
SIMD_INLINE VInt operator<<(VInt a, uint32_t b) { return _mm512_slli_epi32(a, b); }

SIMD_INLINE VInt operator>>(VInt a, VInt b) { return _mm512_srav_epi32(a, b); }
SIMD_INLINE VInt operator<<(VInt a, VInt b) { return _mm512_sllv_epi32(a, b); }
SIMD_INLINE VInt operator~(VInt a) { return a ^ ~0; }
SIMD_INLINE VInt operator|=(VInt& a, VInt b) { return a = (a | b); }
SIMD_INLINE VInt operator&=(VInt& a, VInt b) { return a = (a | b); }

// Math ops
namespace simd {
inline const VInt RampI = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);

SIMD_INLINE VInt round2i(VFloat x) { return _mm512_cvtps_epi32(x.reg); }
SIMD_INLINE VInt trunc2i(VFloat x) { return _mm512_cvttps_epi32(x.reg); }
SIMD_INLINE VInt floor2i(VFloat x) { return _mm512_cvt_roundps_epi32(x.reg, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); }
SIMD_INLINE VFloat conv2f(VInt x) { return _mm512_cvtepi32_ps(x.reg); }

SIMD_INLINE VFloat floor(VFloat x) { return _mm512_roundscale_ps(x, _MM_FROUND_TO_NEG_INF); }
SIMD_INLINE VFloat ceil(VFloat x) { return _mm512_roundscale_ps(x, _MM_FROUND_TO_POS_INF); }
SIMD_INLINE VFloat round(VFloat x) { return _mm512_roundscale_ps(x, _MM_FROUND_TO_NEAREST_INT); }
SIMD_INLINE VFloat fract(VFloat x) { return _mm512_reduce_ps(x, _MM_FROUND_TO_NEG_INF); }

SIMD_INLINE VInt re2i(VFloat x) { return _mm512_castps_si512(x); }  // reinterpret float bits to int
SIMD_INLINE VFloat re2f(VInt x) { return _mm512_castsi512_ps(x); }  // reinterpret int to float bits

SIMD_INLINE VInt min(VInt x, VInt y) { return _mm512_min_epi32(x, y); }
SIMD_INLINE VInt max(VInt x, VInt y) { return _mm512_max_epi32(x, y); }

SIMD_INLINE VFloat min(VFloat x, VFloat y) { return _mm512_min_ps(x, y); }
SIMD_INLINE VFloat max(VFloat x, VFloat y) { return _mm512_max_ps(x, y); }

//x * y + z
SIMD_INLINE VFloat fma(VFloat x, VFloat y, VFloat z) { return _mm512_fmadd_ps(x, y, z); }
//x * y - z
SIMD_INLINE VFloat fms(VFloat x, VFloat y, VFloat z) { return _mm512_fmsub_ps(x, y, z); }

// Linear interpolation between `a` and `b`: `a*(1-t) + b*t`
// https://fgiesen.wordpress.com/2012/08/15/linear-interpolation-past-present-and-future/
SIMD_INLINE VFloat lerp(VFloat a, VFloat b, VFloat t) { return _mm512_fmadd_ps(t, b, _mm512_fnmadd_ps(t, a, a)); }

SIMD_INLINE VFloat sqrt(VFloat x) { return _mm512_sqrt_ps(x); }
SIMD_INLINE VFloat approx_sqrt(VFloat x) { return _mm512_mul_ps(_mm512_rsqrt14_ps(x), x); }
SIMD_INLINE VFloat approx_rsqrt(VFloat x) { return _mm512_rsqrt14_ps(x); }
SIMD_INLINE VFloat approx_rcp(VFloat x) { return _mm512_rcp14_ps(x); }

SIMD_INLINE VFloat abs(VFloat x) { return _mm512_abs_ps(x); }
SIMD_INLINE VInt abs(VInt x) { return _mm512_abs_epi32(x); }

// lanewise: cond ? a : b
SIMD_INLINE VFloat csel(VMask cond, VFloat a, VFloat b) { return _mm512_mask_mov_ps(b, cond, a); }
SIMD_INLINE VInt csel(VMask cond, VInt a, VInt b) { return _mm512_mask_mov_epi32(b, cond, a); }

// if (cond) dest = x
SIMD_INLINE void set_if(VMask cond, VFloat& dest, VFloat x) { dest = csel(cond, x, dest); }
SIMD_INLINE void set_if(VMask cond, VInt& dest, VInt x) { dest = csel(cond, x, dest); }

SIMD_INLINE bool any(VMask cond) { return cond != 0; }
SIMD_INLINE bool all(VMask cond) { return cond == 0xFFFF; }

// 16-bit linear interpolation with 15-bit interpolant: a + (b - a) * t
// mulhrs(a, b) = (a * b + (1 << 14)) >> 15
SIMD_INLINE VInt lerp16(VInt a, VInt b, VInt t) { return _mm512_add_epi16(a, _mm512_mulhrs_epi16(_mm512_sub_epi16(b, a), t)); }

// shift right logical
SIMD_INLINE VInt shrl(VInt a, uint32_t b) { return _mm512_srli_epi32(a, b); }
SIMD_INLINE VInt shrl(VInt a, VInt b) { return _mm512_srlv_epi32(a, b); }
SIMD_INLINE VInt rotl(VInt a, uint32_t b) { return _mm512_rolv_epi32(a, _mm512_set1_epi32((int32_t)b)); }
SIMD_INLINE VInt rotr(VInt a, uint32_t b) { return _mm512_rorv_epi32(a, _mm512_set1_epi32((int32_t)b)); }

SIMD_INLINE VMask ucmp_ge(VInt a, VInt b) { return _mm512_cmpge_epu32_mask(a, b); }

// Reverse bits of packed 32-bit integers.
// https://wunkolo.github.io/post/2020/11/gf2p8affineqb-bit-reversal/
SIMD_INLINE VInt bitrev(VInt x) {
    const auto A = _mm512_set1_epi64(0b10000000'01000000'00100000'00010000'00001000'00000100'00000010'00000001LL);
    const auto B = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

    auto rev8 = _mm512_gf2p8affine_epi64_epi8(x, A, 0);
    return _mm512_shuffle_epi8(rev8, _mm512_broadcast_i32x4(B));
}
SIMD_INLINE VInt lzcnt(VInt x) { return _mm512_lzcnt_epi32(x); }

// https://en.wikipedia.org/wiki/Find_first_set#Properties_and_relations
// tzcnt(x) = 31 - lzcnt(x & -x)
//          = popcnt((x & -x) - 1)   (popcnt 3c vs lzcnt 4c, avoids const load from mem -> reduced latency)
SIMD_INLINE VInt tzcnt(VInt x) { return _mm512_popcnt_epi32((x & (0 - x)) - 1); }

// Calculate coarse partial derivatives for a 4x4 fragment.
// https://gamedev.stackexchange.com/a/130933
SIMD_INLINE VFloat dFdx(VFloat p) {
    auto a = _mm512_shuffle_ps(p, p, 0b10'10'00'00);  //[0 0 2 2]
    auto b = _mm512_shuffle_ps(p, p, 0b11'11'01'01);  //[1 1 3 3]
    return _mm512_sub_ps(b, a);
}
SIMD_INLINE VFloat dFdy(VFloat p) {
    // auto a = _mm256_permute2x128_si256(p, p, 0b00'00'00'00);  // dupe lower 128 lanes
    // auto b = _mm256_permute2x128_si256(p, p, 0b01'01'01'01);  // dupe upper 128 lanes
    auto a = _mm512_shuffle_f32x4(p, p, 0b10'10'00'00);
    auto b = _mm512_shuffle_f32x4(p, p, 0b11'11'01'01);
    return _mm512_sub_ps(b, a);
}
};  // namespace simd