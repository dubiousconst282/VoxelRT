#include "Havk.h"

// #define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <stdexcept>

namespace havk {

static int GetNumChannels(VkFormat format) {
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

// Adapted from stbi__convert_format
// Changed so that truncating conversion removes RGBA channels rather than switching to gray and/or alpha when req_comp is 1 or 2
// clang-format off
static void ConvertFormat(uint8_t*& srcData, int channels, int width, int height, int destChannels) {
    uint8_t* outData = srcData;
    if (destChannels>channels) {
        outData = (uint8_t*)malloc(size_t(width) * size_t(height) * size_t(destChannels));
    }

    for (int y=0; y < height; ++y) {
        uint8_t* src  = &srcData[y * width * channels];
        uint8_t* dest = &outData[y * width * destChannels];
  
        #define STBI__COMBO(a,b)  ((a)*8+(b))
        #define STBI__CASE(a,b)   case STBI__COMBO(a,b): for(int x=width-1; x >= 0; --x, src += a, dest += b)
        // convert source image with img_n components to one with req_comp components;
        // avoid switch per pixel, so use switch per scanline and massive macros
        switch (STBI__COMBO(channels, destChannels)) {
            STBI__CASE(1,2) { dest[0]=src[0]; dest[1]=255;                                     } break;
            STBI__CASE(1,3) { dest[0]=dest[1]=dest[2]=src[0];                                  } break;
            STBI__CASE(1,4) { dest[0]=dest[1]=dest[2]=src[0]; dest[3]=255;                     } break;
            
            STBI__CASE(2,1) { dest[0]=src[0];                                                  } break;
            STBI__CASE(2,3) { dest[0]=dest[1]=dest[2]=src[0];                                  } break;
            STBI__CASE(2,4) { dest[0]=dest[1]=dest[2]=src[0]; dest[3]=src[1];                  } break;

            STBI__CASE(3,4) { dest[0]=src[0];dest[1]=src[1];dest[2]=src[2];dest[3]=255;        } break;
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

ImagePtr Image::LoadFile(DeviceContext* ctx, std::string_view path, VkImageUsageFlags usage, VkFormat format, uint32_t mipLevels,
                         Future* uploadSync) {
    int width, height, loadedChannels;
    int destChannels = GetNumChannels(format);
    uint8_t* pixels = (uint8_t*)stbi_load(path.data(), &width, &height, &loadedChannels, 0);

    if (loadedChannels != destChannels) {
        ConvertFormat(pixels, loadedChannels, width, height, destChannels);
    }

    ImagePtr image = ctx->CreateImage({
        .Format = format,
        .Usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .Width = uint32_t(width),
        .Height = uint32_t(height),
        .NumLevels = mipLevels,
    });
    Future submitSync = image->Upload(pixels, size_t(width) * size_t(height) * size_t(destChannels));

    if (uploadSync) {
        *uploadSync = std::move(submitSync);
    } else {
        submitSync.Wait();
    }
    stbi_image_free(pixels);

    return image;
}
ImagePtr Image::LoadFilePanoramaToCube(DeviceContext* ctx, std::string_view path, VkImageUsageFlags usage, Future* uploadSync) {
    int width, height;
    float* pixels = stbi_loadf(path.data(), &width, &height, nullptr, 3);
    size_t dataSize = size_t(width) * size_t(height) * (4 * 3);
    uint32_t faceSize = uint32_t(width) / 4;

    ImagePtr cubeImage = ctx->CreateImage({
        .Format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        .Usage = usage | VK_IMAGE_USAGE_STORAGE_BIT,
        .Width = faceSize,
        .Height = faceSize,
        .NumLayers = 6,
    });
    BufferPtr stageBuffer = ctx->CreateBuffer({
        .Size = dataSize,
        .Usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .VmaFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
    });
    stageBuffer->Write(pixels, 0, dataSize);
    stbi_image_free(pixels);

    auto shader = ctx->PipeBuilder->CreateCompute("Havk/PanoramaToCube.slang");
    struct ConvertDispatchParams {
        VkDeviceAddress SourceImage;  // RGB32
        uint32_t SourceImageW, SourceImageH;
        havk::ImageHandle DestCube;
    };

    Future submitSync = ctx->Submit([&](havk::CommandList cmds) {
        ConvertDispatchParams pc = {
            .SourceImage = cmds.GetDeviceAddress(*stageBuffer, UseBarrier::ComputeRead),
            .SourceImageW = uint32_t(width),
            .SourceImageH = uint32_t(height),
            .DestCube = cmds.GetDescriptorHandle(*cubeImage, UseBarrier::ComputeReadWrite, VK_IMAGE_LAYOUT_GENERAL)
        };
        shader->Dispatch(cmds, { (faceSize + 7) / 8, (faceSize + 7) / 8, 6 }, pc);

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