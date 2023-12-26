#include "Rasterizer.h"

namespace swr {

// De-interleave vertex indices via 8x3 transpose - https://stackoverflow.com/a/69083795
static void Transpose8x3(__m256i v[3]) {
    auto a = _mm256_blend_epi32(_mm256_blend_epi32(v[1], v[0], 0b01'001'001), v[2], 0b00'100'100);
    auto b = _mm256_blend_epi32(_mm256_blend_epi32(v[0], v[2], 0b01'001'001), v[1], 0b00'100'100);
    auto c = _mm256_blend_epi32(_mm256_blend_epi32(v[2], v[1], 0b01'001'001), v[0], 0b00'100'100);

    v[0] = _mm256_permutevar8x32_epi32(a, _mm256_setr_epi32(0, 3, 6, 1, 4, 7, 2, 5));
    v[1] = _mm256_permutevar8x32_epi32(b, _mm256_setr_epi32(1, 4, 7, 2, 5, 0, 3, 6));
    v[2] = _mm256_permutevar8x32_epi32(c, _mm256_setr_epi32(2, 5, 0, 3, 6, 1, 4, 7));
}

// De-interleave vertex indices via 16x3 transpose - https://stackoverflow.com/a/45025712
static void Transpose16x3(__m512i v[3]) {

    //   0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
    //  16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
    //  32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47

    __m512i a0, a1, a2, r0, r1, r2;

    a0 = _mm512_shuffle_i64x2(v[0], v[1], _MM_SHUFFLE(3, 2, 1, 0));
    a1 = _mm512_shuffle_i64x2(v[0], v[2], _MM_SHUFFLE(1, 0, 3, 2));
    a2 = _mm512_shuffle_i64x2(v[1], v[2], _MM_SHUFFLE(3, 2, 1, 0));

    //   0  1  2  3  4  5  6  7 24 25 26 27 28 29 30 31
    //   8  9 10 11 12 13 14 15 32 33 34 35 36 37 38 39
    //  16 17 18 19 20 21 22 23 40 41 42 43 44 45 46 47

    r0 = _mm512_mask_blend_epi32(0xf0f0, a0, a1);
    r1 = _mm512_permutex2var_epi32(a0, _mm512_setr_epi32(4, 5, 6, 7, 16, 17, 18, 19, 12, 13, 14, 15, 24, 25, 26, 27), a2);
    r2 = _mm512_mask_blend_epi32(0xf0f0, a1, a2);

    //   0  1  2  3 12 13 14 15 24 25 26 27 36 37 38 39
    //   4  5  6  7 16 17 18 19 28 29 30 31 40 41 42 43
    //   8  9 10 11 20 21 22 23 32 33 34 35 44 45 46 47

    a0 = _mm512_mask_blend_epi32(0xcccc, r0, r1);
    a1 = _mm512_castps_si512(_mm512_shuffle_ps(_mm512_castsi512_ps(r0), _mm512_castsi512_ps(r2), 78));
    a2 = _mm512_mask_blend_epi32(0xcccc, r1, r2);

    //   0  1  6  7 12 13 18 19 24 25 30 31 36 37 42 43
    //   2  3  8  9 14 15 20 21 26 27 32 33 38 39 44 45
    //   4  5 10 11 16 17 22 23 28 29 34 35 40 41 46 47

    v[0] = _mm512_mask_blend_epi32(0xaaaa, a0, a1);
    v[1] = _mm512_permutex2var_epi32(a0, _mm512_setr_epi32(1, 16, 3, 18, 5, 20, 7, 22, 9, 24, 11, 26, 13, 28, 15, 30), a2);
    v[2] = _mm512_mask_blend_epi32(0xaaaa, a1, a2);

    //   0  3  6  9 12 15 18 21 24 27 30 33 36 39 42 45
    //   1  4  7 10 13 16 19 22 25 28 31 34 37 40 43 46
    //   2  5  8 11 14 17 20 23 26 29 32 35 38 41 44 47
}

VInt VertexReader::ReadIndices(size_t offset) {
    VInt indices;

    switch (IndexFormat) {
        case U32: indices = _mm512_loadu_epi32(&IndexBuffer[offset * 4]); break;
        case U16: indices = _mm512_cvtepu16_epi32(_mm256_loadu_epi16(&IndexBuffer[offset * 2])); break;
        case U8: indices = _mm512_cvtepu8_epi32(_mm_loadu_epi8(&IndexBuffer[offset * 1])); break;
        default: assert(!"Unknown index buffer format");
    }
    // Mask-out reads beyond buffer size to zero to prevent rasterizer from rendering garbage
    if (offset + VInt::Length > Count) {
        indices = _mm512_maskz_mov_epi32(_mm512_cmplt_epi32_mask(VInt::ramp(), VInt(Count - offset)), indices);
    }
    return indices;
}

void VertexReader::ReadTriangleIndices(size_t offset, VInt indices[3]) {
    for (uint32_t i = 0; i < 3; i++) {
        indices[i] = ReadIndices(offset + i * 16);
    }
    Transpose16x3(&indices->reg);
}

}; //namespace swr