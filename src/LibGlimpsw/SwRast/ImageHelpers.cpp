#include "Texture.h"

#include <stdexcept>

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
        throw std::runtime_error("Failed to load image");
    }
    return {
        .Width = (uint32_t)width,
        .Height = (uint32_t)height,
        .Type = type,
        .Data = { pixels, stbi_image_free },
    };
}

StbImage StbImage::Create(uint32_t width, uint32_t height) {
    return {
        .Width = width,
        .Height = height,
        .Type = PixelType::RGBA_U8,
        .Data = { (uint8_t*)std::malloc(width * height * 4), &std::free }
    };
}

void StbImage::SavePng(std::string_view path) {
    if (Type != PixelType::RGBA_U8) {
        throw std::runtime_error("Unsupported pixel format");
    }
    stbi_write_png(path.data(), (int)Width, (int)Height, 4, Data.get(), (int)Width * 4);
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

    for (uint32_t y = 0; y < tex.Height; y += simd::TileHeight) {
        for (uint32_t x = 0; x < tex.Width; x += simd::TileWidth) {
            VFloat3 tile;

            for (uint32_t sy = 0; sy < simd::TileHeight; sy++) {
                for (uint32_t sx = 0; sx < simd::TileWidth; sx++) {
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
        for (uint32_t y = 0; y < faceSize; y += simd::TileHeight) {
            for (uint32_t x = 0; x < faceSize; x += simd::TileWidth) {
                float scaleUV = 1.0f / (faceSize - 1);
                VFloat u = simd::conv2f((int32_t)x + simd::TileOffsetsX) * scaleUV;
                VFloat v = simd::conv2f((int32_t)y + simd::TileOffsetsY) * scaleUV;

                VFloat3 dir = UnprojectCubemap(u, v, (int32_t)layer);

                for (uint32_t i = 0; i < VFloat::Length; i++) {
                    u[i] = atan2f(dir.z[i], dir.x[i]) / simd::tau + 0.5f;
                    v[i] = asinf(-dir.y[i]) / simd::pi + 0.5f;
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

};  // namespace swr