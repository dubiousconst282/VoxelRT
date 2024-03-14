#pragma once

struct VInt;
using VMask = VInt;

struct VInt {
    static const uint32_t Length = sizeof(__m256i) / sizeof(int32_t);

    __m256i reg;

    SIMD_INLINE VInt() { reg = _mm256_set1_epi32(0); }
    SIMD_INLINE VInt(__m256i x) { reg = x; }
    SIMD_INLINE VInt(int32_t x) { reg = _mm256_set1_epi32(x); }
    SIMD_INLINE operator __m256i() const { return reg; }

    SIMD_INLINE int32_t& operator[](size_t idx) const {
        assert(idx < Length);
        return ((int32_t*)&reg)[idx];
    }

    SIMD_INLINE static VInt load(const void* ptr) { return _mm256_loadu_epi32(ptr); }
    SIMD_INLINE void store(void* ptr) const { _mm256_storeu_epi32(ptr, reg); }

    SIMD_INLINE static VInt mask_load(const void* ptr, VMask mask) { return _mm256_maskload_epi32((int*)ptr, mask); }
    SIMD_INLINE void mask_store(void* ptr, VMask mask) { _mm256_maskstore_epi32((int*)ptr, mask, reg); }

    template<int IndexScale = 4>
    SIMD_INLINE static VInt gather(const void* basePtr, VInt indices) {
        return _mm256_i32gather_epi32((int*)basePtr, indices.reg, IndexScale);
    }
    template<int IndexScale = 4>
    SIMD_INLINE static VInt mask_gather(const void* basePtr, VInt indices, VMask mask) {
        return _mm256_mask_i32gather_epi32(_mm256_set1_epi32(0), (int*)basePtr, indices.reg, mask, IndexScale);
    }

    SIMD_INLINE static VInt shuffle(VInt table, VInt index) { return _mm256_permutevar8x32_epi32(table, index); }
};
struct VFloat {
    static const uint32_t Length = sizeof(__m256) / sizeof(float);

    __m256 reg;

    SIMD_INLINE VFloat() { reg = _mm256_set1_ps(0.0f); }
    SIMD_INLINE VFloat(__m256 x) { reg = x; }
    SIMD_INLINE VFloat(float x) { reg = _mm256_set1_ps(x); }
    SIMD_INLINE operator __m256() const { return reg; }

    SIMD_INLINE float& operator[](size_t idx) const {
        assert(idx < Length);
        return ((float*)&reg)[idx];
    }

    SIMD_INLINE static VFloat load(const void* ptr) { return _mm256_loadu_ps((float*)ptr); }
    SIMD_INLINE void store(void* ptr) const { _mm256_storeu_ps((float*)ptr, reg); }

    SIMD_INLINE static VFloat mask_load(const void* ptr, VMask mask) { return _mm256_maskload_ps((float*)ptr, mask); }
    SIMD_INLINE void mask_store(void* ptr, VMask mask) { _mm256_maskstore_ps((float*)ptr, mask, reg); }

    template<int IndexScale = 4>
    SIMD_INLINE static VFloat gather(const void* basePtr, VInt indices) {
        return _mm256_i32gather_ps((float*)basePtr, indices.reg, IndexScale);
    }
    template<int IndexScale = 4>
    SIMD_INLINE static VFloat mask_gather(const void* basePtr, VInt indices, VMask mask) {
        return _mm256_mask_i32gather_ps(_mm256_set1_ps(0), (float*)basePtr, indices.reg, mask, IndexScale);
    }
};

#define _AVX2_CMP_OP(op, vtype, etype)                         \
    SIMD_INLINE VMask operator op(vtype a, vtype b) {          \
        typedef __attribute((vector_size(32))) etype nvtype__; \
        return ((nvtype__)a)op((nvtype__)b);                   \
    }

#define _SIMD_DEF_OPERATORS(V, E, OpSuffix, MulOp, BitSuffix)                  \
    SIMD_INLINE V operator+(V a, V b) { return _mm256_add_##OpSuffix(a, b); }  \
    SIMD_INLINE V operator-(V a, V b) { return _mm256_sub_##OpSuffix(a, b); }  \
    SIMD_INLINE V operator*(V a, V b) { return _mm256_##MulOp(a, b); }         \
    SIMD_INLINE V operator&(V a, V b) { return _mm256_and_##BitSuffix(a, b); } \
    SIMD_INLINE V operator|(V a, V b) { return _mm256_or_##BitSuffix(a, b); }  \
    SIMD_INLINE V operator^(V a, V b) { return _mm256_xor_##BitSuffix(a, b); } \
                                                                               \
    SIMD_INLINE V operator+=(V& a, V b) { return a = (a + b); }                \
    SIMD_INLINE V operator-=(V& a, V b) { return a = (a - b); }                \
    SIMD_INLINE V operator*=(V& a, V b) { return a = (a * b); }                \
    _AVX2_CMP_OP(<, V, E)                                                      \
    _AVX2_CMP_OP(>, V, E)                                                      \
    _AVX2_CMP_OP(<=, V, E)                                                     \
    _AVX2_CMP_OP(>=, V, E)                                                     \
    _AVX2_CMP_OP(==, V, E)                                                     \
    _AVX2_CMP_OP(!=, V, E)

_SIMD_DEF_OPERATORS(VFloat, float, ps, mul_ps, ps);
SIMD_INLINE VFloat operator/(VFloat a, VFloat b) { return _mm256_div_ps(a, b); }
SIMD_INLINE VFloat operator-(VFloat a) { return a ^ -0.0f; }

_SIMD_DEF_OPERATORS(VInt, int32_t, epi32, mullo_epi32, si256);
SIMD_INLINE VInt operator>>(VInt a, uint32_t b) { return _mm256_srai_epi32(a, (int)b); }
SIMD_INLINE VInt operator<<(VInt a, uint32_t b) { return _mm256_slli_epi32(a, (int)b); }

SIMD_INLINE VInt operator>>(VInt a, VInt b) { return _mm256_srav_epi32(a, b); }
SIMD_INLINE VInt operator<<(VInt a, VInt b) { return _mm256_sllv_epi32(a, b); }
SIMD_INLINE VInt operator~(VInt a) { return a ^ ~0; }
SIMD_INLINE VInt operator|=(VInt& a, VInt b) { return a = (a | b); }
SIMD_INLINE VInt operator&=(VInt& a, VInt b) { return a = (a | b); }

// Math ops
namespace simd {
inline const VInt RampI = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);

SIMD_INLINE VInt round2i(VFloat x) { return _mm256_cvtps_epi32(x.reg); }
SIMD_INLINE VInt trunc2i(VFloat x) { return _mm256_cvttps_epi32(x.reg); }
SIMD_INLINE VInt floor2i(VFloat x) { return _mm256_cvtps_epi32(_mm256_floor_ps(x.reg)); }
SIMD_INLINE VFloat conv2f(VInt x) { return _mm256_cvtepi32_ps(x.reg); }

SIMD_INLINE VFloat floor(VFloat x) { return _mm256_round_ps(x, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC); }
SIMD_INLINE VFloat ceil(VFloat x) { return _mm256_round_ps(x, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC); }
SIMD_INLINE VFloat round(VFloat x) { return _mm256_round_ps(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC); }
SIMD_INLINE VFloat fract(VFloat x) { return x - floor(x); }

SIMD_INLINE VInt re2i(VFloat x) { return _mm256_castps_si256(x); }  // reinterpret float bits to int
SIMD_INLINE VFloat re2f(VInt x) { return _mm256_castsi256_ps(x); }  // reinterpret int to float bits

SIMD_INLINE VInt min(VInt x, VInt y) { return _mm256_min_epi32(x, y); }
SIMD_INLINE VInt max(VInt x, VInt y) { return _mm256_max_epi32(x, y); }

SIMD_INLINE VFloat min(VFloat x, VFloat y) { return _mm256_min_ps(x, y); }
SIMD_INLINE VFloat max(VFloat x, VFloat y) { return _mm256_max_ps(x, y); }

//x * y + z
SIMD_INLINE VFloat fma(VFloat x, VFloat y, VFloat z) { return _mm256_fmadd_ps(x, y, z); }
//x * y - z
SIMD_INLINE VFloat fms(VFloat x, VFloat y, VFloat z) { return _mm256_fmsub_ps(x, y, z); }

// Linear interpolation between `a` and `b`: `a*(1-t) + b*t`
// https://fgiesen.wordpress.com/2012/08/15/linear-interpolation-past-present-and-future/
SIMD_INLINE VFloat lerp(VFloat a, VFloat b, VFloat t) { return _mm256_fmadd_ps(t, b, _mm256_fnmadd_ps(t, a, a)); }

SIMD_INLINE VFloat sqrt(VFloat x) { return _mm256_sqrt_ps(x); }
SIMD_INLINE VFloat approx_sqrt(VFloat x) { return _mm256_mul_ps(_mm256_rsqrt_ps(x), x); }
SIMD_INLINE VFloat approx_rsqrt(VFloat x) { return _mm256_rsqrt_ps(x); }
SIMD_INLINE VFloat approx_rcp(VFloat x) { return _mm256_rcp_ps(x); }

SIMD_INLINE VFloat abs(VFloat x) { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), x); }
SIMD_INLINE VInt abs(VInt x) { return _mm256_abs_epi32(x); }

// lanewise: cond ? a : b
SIMD_INLINE VFloat csel(VMask cond, VFloat a, VFloat b) { return _mm256_blendv_ps(b, a, cond); }
SIMD_INLINE VInt csel(VMask cond, VInt a, VInt b) { return _mm256_blendv_epi8(b, a, cond); }

// if (cond) dest = x
SIMD_INLINE void set_if(VMask cond, VFloat& dest, VFloat x) { dest = csel(cond, x, dest); }
SIMD_INLINE void set_if(VMask cond, VInt& dest, VInt x) { dest = csel(cond, x, dest); }

SIMD_INLINE bool any(VMask cond) { return _mm256_movemask_epi8(cond) != 0; }
SIMD_INLINE bool all(VMask cond) { return _mm256_movemask_epi8(cond) == 0xFFFFFFFF; }

// 16-bit linear interpolation with 15-bit interpolant: a + (b - a) * t
// mulhrs(a, b) = (a * b + (1 << 14)) >> 15
SIMD_INLINE VInt lerp16(VInt a, VInt b, VInt t) { return _mm256_add_epi16(a, _mm256_mulhrs_epi16(_mm256_sub_epi16(b, a), t)); }

// shift right logical
SIMD_INLINE VInt shrl(VInt a, uint32_t b) { return _mm256_srli_epi32(a, (int)b); }
SIMD_INLINE VInt shrl(VInt a, VInt b) { return _mm256_srlv_epi32(a, b); }
// UB for `count` == 0 or 31.
SIMD_INLINE VInt rotl(VInt a, uint32_t b) { return (a << b) | (a >> (32 - b)); }
SIMD_INLINE VInt rotr(VInt a, uint32_t b) { return (a >> b) | (a << (32 - b)); }

// unsigned a >= b
SIMD_INLINE VMask ucmp_ge(VInt a, VInt b) {
    typedef __attribute((vector_size(32))) uint32_t vuint_t;
    return ((vuint_t)a) >= ((vuint_t)b);
}

// TODO
SIMD_INLINE VInt bitrev(VInt x);
SIMD_INLINE VInt lzcnt(VInt x);
SIMD_INLINE VInt tzcnt(VInt x);

// Calculate coarse partial derivatives for a 4x2 fragment.
// https://gamedev.stackexchange.com/a/130933
SIMD_INLINE VFloat dFdx(VFloat p) {
    auto a = _mm256_shuffle_ps(p, p, 0b10'10'00'00);  //[0 0 2 2]
    auto b = _mm256_shuffle_ps(p, p, 0b11'11'01'01);  //[1 1 3 3]
    return _mm256_sub_ps(b, a);
}
SIMD_INLINE VFloat dFdy(VFloat p) {
    auto a = _mm256_permute2x128_si256(p, p, 0b00'00'00'00);  // dupe lower 128 lanes
    auto b = _mm256_permute2x128_si256(p, p, 0b01'01'01'01);  // dupe upper 128 lanes
    return _mm256_sub_ps(b, a);
}
};  // namespace simd