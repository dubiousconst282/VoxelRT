#include "OGL/QuickGL.h"
#include "../SwRast/Texture.h"

namespace ogl {

std::unique_ptr<Texture2D> Texture2D::Load(std::string_view path, uint32_t mipLevels, GLenum internalFmt) {
    auto img = swr::StbImage::Load(path);
    auto tex = std::make_unique<Texture2D>(img.Width, img.Height, mipLevels, internalFmt);

    // Why the fuck?
    GLenum fmt = internalFmt != GL_RGBA8 && internalFmt != GL_RG8 ? GL_RGBA_INTEGER : GL_RGBA;
    tex->SetPixels(fmt, GL_UNSIGNED_BYTE, img.Data.get());
    return tex;
}
std::unique_ptr<TextureCube> TextureCube::LoadPanorama(std::string_view path, Shader& panoToCubeShader) {
    auto img = swr::StbImage::Load(path, swr::StbImage::PixelType::RGB_F32);

    uint32_t faceSize = img.Width / 4;
    auto panoTex = Texture2D(img.Width, img.Height, 1, GL_RGBA32F);
    panoTex.SetPixels(GL_RGB, GL_FLOAT, img.Data.get());

    auto cubeTex = std::make_unique<TextureCube>(faceSize, faceSize, 4, GL_R11F_G11F_B10F);

    panoToCubeShader.SetUniform("u_SourceImage", panoTex);
    panoToCubeShader.SetUniform("u_DestCube", *cubeTex);
    panoToCubeShader.DispatchCompute((faceSize + 7) / 8, (faceSize + 7) / 8, 6);

    glGenerateTextureMipmap(cubeTex->Handle);

    return cubeTex;
}

};  // namespace ogl