#pragma once

#include <cstdint>
#include <string_view>
#include <memory>

namespace havx {

struct DecodedImage {
    enum class PixelType { Empty, U8 = 8, U16 = 16, F32 = 32 };
    using Deleter = void(*)(void*);

    uint32_t Width = 0, Height = 0;
    uint32_t NumChannels = 0;
    PixelType Type = PixelType::Empty;
    std::unique_ptr<uint8_t[], Deleter> Data = { nullptr, &std::free };

    static DecodedImage LoadFromFile(std::string_view path, PixelType type = PixelType::U8, uint32_t numChannels = 4);
    static DecodedImage LoadFromMemory(const uint8_t* data, size_t size, PixelType type = PixelType::U8, uint32_t numChannels = 4);

    uint32_t GetBytesPerPixel() const { return NumChannels * ((uint32_t)Type / 8); }
};

};