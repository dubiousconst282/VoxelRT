#include "SwRast.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace swr {

Texture2D Texture2D::LoadImage(std::string_view path, uint32_t mipLevels) {
    int width, height, channels;
    stbi_uc* pixels = stbi_load(path.data(), &width, &height, &channels, 4);

    auto tex = Texture2D((uint32_t)width, (uint32_t)height, mipLevels);
    tex.SetPixels((uint32_t*)pixels, tex.Width);

    stbi_image_free(pixels);

    return tex;
}

Texture2D Texture2D::LoadNormalMap(std::string_view normalPath, std::string_view metallicRoughnessPath, uint32_t mipLevels) {
    int width, height, channels;
    stbi_uc* pixels = stbi_load(normalPath.data(), &width, &height, &channels, 4);
    stbi_uc* mrPixels = nullptr;

    if (!metallicRoughnessPath.empty()) {
        int mrWidth, mrHeight, mrChannels;
        mrPixels = stbi_load(metallicRoughnessPath.data(), &mrWidth, &mrHeight, &mrChannels, 4);

        if (mrWidth != width || mrHeight != height) {
            throw std::exception("Bad Metallic-Roughness map");
        }
    }

    for (uint32_t i = 0; i < width * height; i++) {
        // Re-normalize to get rid of JPEG artifacts
        glm::vec3 N = glm::vec3(pixels[i * 4 + 0], pixels[i * 4 + 1], pixels[i * 4 + 2]);
        N = glm::normalize(N / 127.0f - 1.0f) * 127.0f + 127.0f;

        pixels[i * 4 + 0] = (uint8_t)roundf(N.x);
        pixels[i * 4 + 1] = (uint8_t)roundf(N.y);

        // Overwrite BA channels from normal map with Metallic and Roughness.
        // The normal Z can be reconstructed with `sqrt(1.0f - dot(n.xy, n.xy))`
        if (mrPixels != nullptr) {
            pixels[i * 4 + 2] = mrPixels[i * 4 + 2];  // Metallic
            pixels[i * 4 + 3] = mrPixels[i * 4 + 1];  // Roughness
        }
    }
    
    auto tex = Texture2D((uint32_t)width, (uint32_t)height, mipLevels);
    tex.SetPixels((uint32_t*)pixels, tex.Width);

    stbi_image_free(pixels);
    stbi_image_free(mrPixels);

    return tex;
}

HdrTexture2D HdrTexture2D::LoadImage(std::string_view filename) {
    int width, height, channels;
    float* pixels = stbi_loadf(filename.data(), &width, &height, &channels, 3);

    auto tex = HdrTexture2D((uint32_t)width, (uint32_t)height, 1);
    tex.SetPixels(pixels, tex.Width, 0);

    stbi_image_free(pixels);

    return tex;
}
HdrTexture2D HdrTexture2D::LoadCubemapFromPanorama(std::string_view filename) {
    int width, height, channels;
    float* pixels = stbi_loadf(filename.data(), &width, &height, &channels, 3);

    uint32_t faceSize = (uint32_t)width / 4;

    auto tex = HdrTexture2D(faceSize, faceSize, 6);
    auto temp = std::make_unique<float[]>(faceSize * faceSize * 3);

    for (uint32_t layer = 0; layer < 6; layer++) {
        for (uint32_t y = 0; y < faceSize; y++) {
            for (uint32_t x = 0; x < faceSize; x++) {
                float u = x / (float)(faceSize - 1) * 2.0f - 1.0f;
                float v = y / (float)(faceSize - 1) * 2.0f - 1.0f;

                // clang-format off
                glm::vec3 dirs[6]{
                    {  1,  v,  u }, 
                    { -1,  v,  u }, 
                    {  u,  1,  v }, 
                    {  u, -1,  v }, 
                    {  u,  v,  1 }, 
                    {  u,  v, -1 },
                };
                glm::vec3 dir = glm::normalize(dirs[layer]);

                float tu = std::atan2f(dir.z, dir.x) / (simd::pi * 2.0f) + 0.5f;
                float tv = std::asinf(-dir.y) / simd::pi + 0.5f;

                // TODO: re-use linear sampling from HdrTextures if/when they support it
                uint32_t px = std::min((uint32_t)(tu * width), (uint32_t)width - 1);
                uint32_t py = std::min((uint32_t)(tv * height), (uint32_t)height - 1);

                for (uint32_t ch = 0; ch < 3; ch++) {
                    temp[(x + y * faceSize) * 3 + ch] = pixels[(px + py * (uint32_t)width) * 3 + ch];
                }
            }
        }
        tex.SetPixels(temp.get(), faceSize, layer);
    }

    stbi_image_free(pixels);

    return tex;
}

void Framebuffer::GetPixels(uint32_t* __restrict dest, uint32_t stride) const {
    for (uint32_t y = 0; y < Height; y += 4) {
        for (uint32_t x = 0; x < Width; x += 4) {
            // Clang is doing some really funky vectorization with this loop. Manual vectorization ftw I guess...
            // for (uint32_t sx = 0; sx < 4; sx++) {
            //     dest[y * stride + x + sx] = src[sx];
            // }
            uint32_t* src = &ColorBuffer[GetPixelOffset(x, y)];

            __m512i tile = _mm512_load_si512(src);
            _mm_storeu_si128((__m128i*)&dest[(y + 0) * stride + x], _mm512_extracti32x4_epi32(tile, 0));
            _mm_storeu_si128((__m128i*)&dest[(y + 1) * stride + x], _mm512_extracti32x4_epi32(tile, 1));
            _mm_storeu_si128((__m128i*)&dest[(y + 2) * stride + x], _mm512_extracti32x4_epi32(tile, 2));
            _mm_storeu_si128((__m128i*)&dest[(y + 3) * stride + x], _mm512_extracti32x4_epi32(tile, 3));
        }
    }
}

void Framebuffer::SaveImage(std::string_view filename) const {
    auto pixels = std::make_unique<uint32_t[]>(Width * Height);
    GetPixels(pixels.get(), Width);
    stbi_write_png(filename.data(), (int)Width, (int)Height, 4, pixels.get(), (int)Width * 4);
}

};  // namespace swr