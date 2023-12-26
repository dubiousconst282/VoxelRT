#pragma once

#include <SwRast/Rasterizer.h>

// Hierarchical depth buffer for occlusion culling
// - https://www.rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
// - https://vkguide.dev/docs/gpudriven/compute_culling/
class DepthPyramid {
    swr::AlignedBuffer<float> _storage = nullptr;
    uint32_t _width, _height, _levels;
    uint32_t _offsets[16]{};
    glm::mat4 _viewProj;

    void EnsureStorage(uint32_t width, uint32_t height);

public:
    void Update(const swr::Framebuffer& fb, const glm::mat4& viewProj);

    float GetDepth(float u, float v, float lod) const;

    bool IsVisible(const glm::vec3 bounds[2], const glm::mat4& transform) const;

    float* GetMipBuffer(uint32_t level, uint32_t& width, uint32_t& height) {
        width = _width >> level;
        height = _height >> level;
        return &_storage[_offsets[level]];
    }

    swr::VFloat __vectorcall SampleDepth(swr::VFloat x, swr::VFloat y, uint32_t level) const {
        swr::VInt ix = swr::simd::round2i(x * (int32_t)_width);
        swr::VInt iy = swr::simd::round2i(y * (int32_t)_height);

        return SampleDepth(ix << 1, iy << 1, level);
    }
    swr::VFloat __vectorcall SampleDepth(swr::VInt ix, swr::VInt iy, uint32_t level) const {
        ix = ix >> 1, iy = iy >> 1;
        uint16_t boundMask =
            _mm512_cmplt_epu32_mask(ix, swr::VInt((int32_t)_width)) & _mm512_cmplt_epu32_mask(iy, swr::VInt((int32_t)_height));
        swr::VInt indices = (ix >> level) + (iy >> level) * (int32_t)(_width >> level);

        return _mm512_mask_i32gather_ps(_mm512_set1_ps(1.0f), boundMask, indices, &_storage[_offsets[level]], 4);
    }
};