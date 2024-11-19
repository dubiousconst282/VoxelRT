#include "Havk.h"

#if __clang__
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wsign-conversion"
    #define STB_IMAGE_IMPLEMENTATION
    #include <stb_image.h>
  #pragma clang diagnostic pop
#endif

#include <stdexcept>

#include "../Havx/DecodedImage.h"

namespace havk {

static uint32_t GetNumChannels(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB: return 4;

        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8_SRGB: return 2;

        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB: return 1;

        default: throw std::runtime_error("Format not supported");
    }
}

ImagePtr Image::LoadFile(DeviceContext* ctx, std::string_view path, VkImageUsageFlags usage, VkFormat format, uint32_t mipLevels,
                         Future* uploadSync) {
    auto decodedImg = havx::DecodedImage::LoadFromFile(path, havx::DecodedImage::PixelType::U8, GetNumChannels(format));

    ImagePtr image = ctx->CreateImage({
        .Format = format,
        .Usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .Width = decodedImg.Width,
        .Height = decodedImg.Height,
        .NumLevels = mipLevels,
    });
    Future submitSync = image->Upload(decodedImg.Data.get(), decodedImg.Width * decodedImg.Height * decodedImg.GetBytesPerPixel());

    if (uploadSync) {
        *uploadSync = std::move(submitSync);
    } else {
        submitSync.Wait();
    }

    return image;
}
ImagePtr Image::LoadFilePanoramaToCube(DeviceContext* ctx, std::string_view path, VkImageUsageFlags usage, Future* uploadSync) {
    havx::DecodedImage panoImg = havx::DecodedImage::LoadFromFile(path, havx::DecodedImage::PixelType::F32, 3);
    size_t dataSize = panoImg.Width * panoImg.Height * sizeof(float) * 3;
    uint32_t faceSize = panoImg.Width / 4;

    ImagePtr cubeImage = ctx->CreateImage({
        .Format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        .Usage = usage | VK_IMAGE_USAGE_STORAGE_BIT,
        .Width = faceSize,
        .Height = faceSize,
        .NumLayers = 6,
        .ViewType = VK_IMAGE_VIEW_TYPE_CUBE,
    });
    BufferPtr stageBuffer = ctx->CreateBuffer({
        .Size = dataSize,
        .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .AllocType = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    });
    stageBuffer->Write(panoImg.Data.get(), 0, dataSize);

    auto shader = ctx->PipeBuilder->CreateCompute("Havk/PanoramaToCube.slang");

    Future submitSync = ctx->Submit([&](havk::CommandList cmds) {
        struct ConvertParams {
            VkDeviceAddress SourceImage;  // RGB32
            uint32_t SourceImageW, SourceImageH;
            havk::ImageHandle DestCube;
        };
        shader->Dispatch(cmds, { (faceSize + 7) / 8, (faceSize + 7) / 8, 6 }, ConvertParams {
            .SourceImage = cmds.GetDeviceAddress(*stageBuffer, UseBarrier::ComputeRead),
            .SourceImageW = panoImg.Width,
            .SourceImageH = panoImg.Height,
            .DestCube = cmds.GetDescriptorHandle(*cubeImage, UseBarrier::ComputeReadWrite, VK_IMAGE_LAYOUT_GENERAL)
        });

        auto naturalLayout = (usage & VK_IMAGE_USAGE_STORAGE_BIT) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cmds.TransitionLayout(*cubeImage, naturalLayout, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    });

    if (uploadSync) {
        *uploadSync = std::move(submitSync);
    } else {
        submitSync.Wait();
    }

    return cubeImage;
}

};  // namespace havk

namespace havx {
  
// Adapted from stbi__convert_format
// Changed so that truncating conversion removes RGBA channels rather than switching to gray and/or alpha when req_comp is 1 or 2
// clang-format off
template<typename T>
static void ConvertFormat(T*& srcData, int channels, int width, int height, int destChannels) {
    constexpr T alpha_one = std::is_floating_point_v<T> ? T(1.0f) : std::numeric_limits<T>::max();

    T* outData = srcData;
    if (destChannels>channels) {
        outData = (T*)malloc(size_t(width) * size_t(height) * size_t(destChannels) * sizeof(T));
    }

    for (int y=0; y < height; ++y) {
        T* src  = &srcData[y * width * channels];
        T* dest = &outData[y * width * destChannels];
  
        #define STBI__COMBO(a,b)  ((a)*8+(b))
        #define STBI__CASE(a,b)   case STBI__COMBO(a,b): for(int x=width-1; x >= 0; --x, src += a, dest += b)
        // convert source image with img_n components to one with req_comp components;
        // avoid switch per pixel, so use switch per scanline and massive macros
        switch (STBI__COMBO(channels, destChannels)) {
            STBI__CASE(1,2) { dest[0]=src[0]; dest[1]=alpha_one;                               } break;
            STBI__CASE(1,3) { dest[0]=dest[1]=dest[2]=src[0];                                  } break;
            STBI__CASE(1,4) { dest[0]=dest[1]=dest[2]=src[0]; dest[3]=alpha_one;               } break;
            
            STBI__CASE(2,1) { dest[0]=src[0];                                                  } break;
            STBI__CASE(2,3) { dest[0]=dest[1]=dest[2]=src[0];                                  } break;
            STBI__CASE(2,4) { dest[0]=dest[1]=dest[2]=src[0]; dest[3]=src[1];                  } break;

            STBI__CASE(3,4) { dest[0]=src[0];dest[1]=src[1];dest[2]=src[2];dest[3]=alpha_one;  } break;
            STBI__CASE(3,1) { dest[0]=src[0];                                                  } break;
            STBI__CASE(3,2) { dest[0]=src[0],dest[1]=src[1];                                   } break;

            STBI__CASE(4,1) { dest[0]=src[0];                                                  } break;
            STBI__CASE(4,2) { dest[0]=src[0];dest[1]=src[1];                                   } break;
            STBI__CASE(4,3) { dest[0]=src[0];dest[1]=src[1];dest[2]=src[2];                    } break;
            default: throw std::runtime_error("Unsupported format conversion");
        }
        #undef STBI__CASE
    }
    if (outData != srcData) {
        free(srcData);
        srcData = outData;
    }
}
// clang-format on

DecodedImage DecodedImage::LoadFromFile(std::string_view path, PixelType type, uint32_t numChannels) {
    assert(numChannels > 0 && numChannels <= 4);
    
    int width, height, loadedChannels;
    void* pixels;
    
    if (type == PixelType::U8) {
        pixels = stbi_load(path.data(), &width, &height, &loadedChannels, 0);

        if (numChannels != 0 && loadedChannels != numChannels) {
            ConvertFormat(*(uint8_t**)&pixels, loadedChannels, width, height, (int)numChannels);
        }
    }
    else if (type == PixelType::U16) {
        pixels = stbi_load_16(path.data(), &width, &height, &loadedChannels, 0);

        if (numChannels != 0 && loadedChannels != numChannels) {
            ConvertFormat(*(uint16_t**)&pixels, loadedChannels, width, height, (int)numChannels);
        }
    }
    else if (type == PixelType::F32) {
        pixels = stbi_loadf(path.data(), &width, &height, &loadedChannels, 0);

        if (numChannels != 0 && loadedChannels != numChannels) {
            ConvertFormat(*(float**)&pixels, loadedChannels, width, height, (int)numChannels);
        }
    }
    else assert(!"Invalid pixel type");

    return DecodedImage {
        .Width = uint32_t(width),
        .Height = uint32_t(height),
        .NumChannels = numChannels == 0 ? uint32_t(loadedChannels) : numChannels,
        .Type = type,
        .Data = { (uint8_t*)pixels, stbi_image_free }
    };
}
DecodedImage DecodedImage::LoadFromMemory(const uint8_t* data, size_t size, PixelType type, uint32_t numChannels) {
    assert(numChannels > 0 && numChannels <= 4);
    
    int width, height, loadedChannels;
    void* pixels;
    
    if (type == PixelType::U8) {
        pixels = stbi_load_from_memory(data, (int)size, &width, &height, &loadedChannels, 0);

        if (numChannels != 0 && loadedChannels != numChannels) {
            ConvertFormat(*(uint8_t**)&pixels, loadedChannels, width, height, (int)numChannels);
        }
    }
    else if (type == PixelType::U16) {
        pixels = stbi_load_16_from_memory(data, (int)size, &width, &height, &loadedChannels, 0);

        if (numChannels != 0 && loadedChannels != numChannels) {
            ConvertFormat(*(uint16_t**)&pixels, loadedChannels, width, height, (int)numChannels);
        }
    }
    else if (type == PixelType::F32) {
        pixels = stbi_loadf_from_memory(data, (int)size, &width, &height, &loadedChannels, 0);

        if (numChannels != 0 && loadedChannels != numChannels) {
            ConvertFormat(*(float**)&pixels, loadedChannels, width, height, (int)numChannels);
        }
    }
    else assert(!"Invalid pixel type");

    return DecodedImage {
        .Width = uint32_t(width),
        .Height = uint32_t(height),
        .NumChannels = numChannels == 0 ? uint32_t(loadedChannels) : numChannels,
        .Type = type,
        .Data = { (uint8_t*)pixels, stbi_image_free }
    };
}
  
};