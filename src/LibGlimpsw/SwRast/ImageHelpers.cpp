#include "Rasterizer.h"
#include "Texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace swr {

StbImage StbImage::Load(std::string_view path, PixelType type) {
    int width, height;
    uint8_t* pixels = type == PixelType::RGBA_U8 ? (uint8_t*)stbi_load(path.data(), &width, &height, nullptr, 4) :
                      type == PixelType::RGB_F32 ? (uint8_t*)stbi_loadf(path.data(), &width, &height, nullptr, 4) :
                                                   nullptr;

    if (pixels == nullptr) {
        throw std::exception("Failed to load image");
    }
    return {
        .Width = (uint32_t)width,
        .Height = (uint32_t)height,
        .Type = type,
        .Data = { pixels, stbi_image_free },
    };
}

namespace texutil {

RgbaTexture2D LoadImage(std::string_view path, uint32_t mipLevels) {
    int width, height, channels;
    stbi_uc* pixels = stbi_load(path.data(), &width, &height, &channels, 4);

    auto tex = RgbaTexture2D((uint32_t)width, (uint32_t)height, mipLevels, 1);
    tex.SetPixels(pixels, tex.Width, 0);
    tex.GenerateMips();

    stbi_image_free(pixels);
    return tex;
}

HdrTexture2D LoadImageHDR(std::string_view path, uint32_t mipLevels) {
    int width, height, channels;
    float* pixels = stbi_loadf(path.data(), &width, &height, &channels, 3);

    auto tex = HdrTexture2D((uint32_t)width, (uint32_t)height, mipLevels, 1);

    for (uint32_t y = 0; y < tex.Height; y += 4) {
        for (uint32_t x = 0; x < tex.Width; x += 4) {
            VFloat3 tile;

            for (uint32_t sy = 0; sy < 4; sy++) {
                for (uint32_t sx = 0; sx < 4; sx++) {
                    uint32_t idx = (x + sx) + (y + sy) * tex.Width;
                    tile.x[sx + sy * 4] = pixels[idx * 3 + 0];
                    tile.y[sx + sy * 4] = pixels[idx * 3 + 1];
                    tile.z[sx + sy * 4] = pixels[idx * 3 + 2];
                }
            }
            tex.WriteTile(swr::pixfmt::R11G11B10f::Pack(tile), x, y);
        }
    }
    stbi_image_free(pixels);

    tex.GenerateMips();

    return tex;
}
HdrTexture2D LoadCubemapFromPanoramaHDR(std::string_view path, uint32_t mipLevels) {
    auto panoTex = LoadImageHDR(path, 1);

    uint32_t faceSize = panoTex.Width / 4;
    auto cubeTex = HdrTexture2D(faceSize, faceSize, mipLevels, 6);

    constexpr SamplerDesc PanoSampler = {
        .Wrap = WrapMode::Repeat,
        .MagFilter = FilterMode::Linear,
        .MinFilter = FilterMode::Linear,
        .EnableMips = false,
    };

    for (uint32_t layer = 0; layer < 6; layer++) {
        for (uint32_t y = 0; y < faceSize; y += 4) {
            for (uint32_t x = 0; x < faceSize; x += 4) {
                float scaleUV = 1.0f / (faceSize - 1);
                VFloat u = simd::conv2f((int32_t)x + FragPixelOffsetsX) * scaleUV;
                VFloat v = simd::conv2f((int32_t)y + FragPixelOffsetsY) * scaleUV;

                VFloat3 dir = UnprojectCubemap(u, v, (int32_t)layer);

                for (uint32_t i = 0; i < VFloat::Length; i++) {
                    u[i] = std::atan2f(dir.z[i], dir.x[i]) / simd::tau + 0.5f;
                    v[i] = std::asinf(-dir.y[i]) / simd::pi + 0.5f;
                }

                VFloat3 tile = panoTex.Sample<PanoSampler>(u, v, (int32_t)layer);
                cubeTex.WriteTile(swr::pixfmt::R11G11B10f::Pack(tile), x, y, layer);
            }
        }
    }

    cubeTex.GenerateMips();
    return cubeTex;
}

};  // namespace texutil

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