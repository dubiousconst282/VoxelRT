#include "OGL/QuickGL.h"
#include "../SwRast/Texture.h"

namespace ogl {

std::unique_ptr<Texture2D> Texture2D::Load(std::string_view path, uint32_t mipLevels, GLenum internalFmt) {
    auto img = swr::StbImage::Load(path);
    auto tex = std::make_unique<Texture2D>(img.Width, img.Height, mipLevels, internalFmt);

    // Why the fuck?
    GLenum fmt = internalFmt == GL_RGBA8I || internalFmt == GL_RGBA8UI ? GL_RGBA_INTEGER : GL_RGBA;
    tex->SetPixels(fmt, GL_UNSIGNED_BYTE, img.Data.get());
    return tex;
}

};  // namespace ogl