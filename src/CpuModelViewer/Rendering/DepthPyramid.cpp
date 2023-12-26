#include "DepthPyramid.h"

#include <algorithm>

float DepthPyramid::GetDepth(float u, float v, float lod) const {
    uint32_t level = std::clamp((uint32_t)lod, 0u, _levels - 1);

    int32_t w = (int32_t)(_width >> level);
    int32_t h = (int32_t)(_height >> level);
    int32_t x = (int32_t)(u * w);
    int32_t y = (int32_t)(v * h);

    const auto Sample = [&](int32_t xo, int32_t yo) {
        int32_t i = std::clamp(x + xo, 0, w - 1) + std::clamp(y + yo, 0, h - 1) * w;
        return _storage[_offsets[level] + (uint32_t)i];
    };
    return std::max({ Sample(0, 0), Sample(1, 0), Sample(0, 1), Sample(1, 1) });
}

bool DepthPyramid::IsVisible(const glm::vec3 bounds[2], const glm::mat4& transform) const {
    if (!_storage) return true;

    glm::vec3 rectMin = glm::vec3(INFINITY), rectMax = glm::vec3(-INFINITY);

    uint8_t combinedOut = 63;
    uint8_t partialOut = 0;

    for (uint32_t i = 0; i < 8; i++) {
        glm::bvec3 corner = { (i >> 0) & 1, (i >> 1) & 1, (i >> 2) & 1 };
        glm::vec4 p = _viewProj * transform * glm::vec4(glm::mix(bounds[0], bounds[1], corner), 1.0f);

        glm::vec3 rp = {
            p.x / p.w * 0.5f + 0.5f,
            p.y / p.w * 0.5f + 0.5f,
            p.z / p.w,
        };
        rectMin = glm::min(rectMin, rp);
        rectMax = glm::max(rectMax, rp);

        uint8_t outcode = 0;
        outcode |= p.x < -p.w ? 1 : 0;
        outcode |= p.x > +p.w ? 2 : 0;
        outcode |= p.y < -p.w ? 4 : 0;
        outcode |= p.y > +p.w ? 8 : 0;
        outcode |= p.z < 0 ? 16 : 0;
        outcode |= p.z > p.w ? 32 : 0;

        combinedOut &= outcode;
        partialOut |= outcode;
    }

    // Hacky frustum check. Cull if all vertices are outside any of the frustum planes.
    // Not that this still have false positives for big objects (see below), but it's good enough for our purposes.
    // - https://bruop.github.io/improved_frustum_culling/
    // - https://iquilezles.org/articles/frustumcorrect/
    if (combinedOut != 0) return false;

    // We don't do clipping, so the ccclusion test wont't work properly with
    // AABBs that are partially out the view frustum.
    // Consider them as visible to prevent flickering.
    if (partialOut != 0) return true;

    float sizeX = (rectMax.x - rectMin.x) * _width;
    float sizeY = (rectMax.y - rectMin.y) * _height;
    float lod = std::ceil(std::log2(std::max(sizeX, sizeY) / 2.0f));

    float screenDepth = GetDepth((rectMin.x + rectMax.x) * 0.5f, (rectMin.y + rectMax.y) * 0.5f, lod);

    // ImGui::GetForegroundDrawList()->AddRect(ImVec2(rectMin.x * _width * 2, (1 - rectMin.y) * _height * 2),
    //                                         ImVec2(rectMax.x * _width * 2, (1 - rectMax.y) * _height * 2),
    //                                         rectMin.z <= screenDepth ? 0x8000FF00 : 0x80FFFFFF);

    return rectMin.z <= screenDepth;
}

void DepthPyramid::Update(const swr::Framebuffer& fb, const glm::mat4& viewProj) {
    EnsureStorage(fb.Width, fb.Height);

    // Downsample original depth buffer
    for (uint32_t y = 0; y < fb.Height; y += 4) {
        for (uint32_t x = 0; x < fb.Width; x += 4) {
            auto tile = _mm512_load_ps(&fb.DepthBuffer[fb.GetPixelOffset(x, y)]);
            // A B C D  ->  max(AC, BD)
            tile = _mm512_shuffle_f32x4(tile, tile, _MM_SHUFFLE(3, 1, 2, 0));
            auto rows = _mm256_max_ps(_mm512_extractf32x8_ps(tile, 0), _mm512_extractf32x8_ps(tile, 1));
            auto cols = _mm256_max_ps(rows, _mm256_movehdup_ps(rows));
            cols = _mm256_permutevar8x32_ps(cols, _mm256_setr_epi32(0, 2, -1, -1, 4, 6, -1, -1));

            _mm_storel_pi((__m64*)&_storage[(x / 2) + (y / 2 + 0) * _width], _mm256_extractf128_ps(cols, 0));
            _mm_storel_pi((__m64*)&_storage[(x / 2) + (y / 2 + 1) * _width], _mm256_extractf128_ps(cols, 1));
        }
    }

    for (uint32_t i = 1; i < _levels; i++) {
        float* src = &_storage[_offsets[i - 1]];
        float* dst = &_storage[_offsets[i + 0]];

        uint32_t w = _width >> (i - 1);
        uint32_t h = _height >> (i - 1);

        // TODO: edge clamping and stuff
        for (uint32_t y = 0; y < h; y += 2) {
            for (uint32_t x = 0; x < w; x += 16) {
                auto rows = _mm512_max_ps(_mm512_loadu_ps(&src[x + (y + 0) * w]), _mm512_loadu_ps(&src[x + (y + 1) * w]));

                auto cols = _mm512_max_ps(rows, _mm512_movehdup_ps(rows));
                auto res = _mm512_permutexvar_ps(_mm512_setr_epi32(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1), cols);

                _mm256_storeu_ps(&dst[(x / 2) + (y / 2) * (w / 2)], _mm512_extractf32x8_ps(res, 0));
            }
        }
    }

    _viewProj = viewProj;
}

void DepthPyramid::EnsureStorage(uint32_t width, uint32_t height) {
    if (_width == width / 2 && _height == height / 2) return;

    _width = width / 2;
    _height = height / 2;
    _levels = (uint32_t)std::bit_width(std::min(_width, _height));
    assert(_levels < 16);

    uint32_t offset = 0;

    for (uint32_t i = 0; i < _levels; i++) {
        _offsets[i] = offset;
        offset += (_width >> i) * (_height >> i);
    }
    _storage = swr::alloc_buffer<float>(offset + 16);
}