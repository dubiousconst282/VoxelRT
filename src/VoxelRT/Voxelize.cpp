#include "VoxelMap.h"

#include <SwRast/Texture.h>
#include <Common/PaletteBuilder.h>

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/math.hpp>
#include <fastgltf/types.hpp>

#include <glm/gtc/type_ptr.hpp>

// http://research.michael-schwarz.com/publ/files/vox-siga10.pdf
static void VoxelizeTriangleSurface(const glm::vec3 v[3], std::function<void(glm::ivec3)> visitor) {
    using namespace glm;

    vec3 e[3] = { v[1] - v[0], v[2] - v[1], v[0] - v[2] };

    vec3 norm = normalize(cross(e[0], e[1]));
    vec3 c = max(sign(norm), 0.0f);  // critical point

    float d1 = dot(norm, c - v[0]);
    float d2 = dot(norm, (1.0f - c) - v[0]);

    vec2 ne[3][3];
    float de[3][3];

    for (uint32_t i = 0; i < 3; i++) {
        ne[0][i] = vec2(-e[i].y, e[i].x) * (norm.z < 0 ? -1.0f : +1.0f);
        de[0][i] = -dot(ne[0][i], vec2(v[i].x, v[i].y)) + max(0.0f, ne[0][i].x) + max(0.0f, ne[0][i].y);

        ne[1][i] = vec2(-e[i].x, e[i].z) * (norm.y < 0 ? -1.0f : +1.0f);
        de[1][i] = -dot(ne[1][i], vec2(v[i].z, v[i].x)) + max(0.0f, ne[1][i].x) + max(0.0f, ne[1][i].y);

        ne[2][i] = vec2(-e[i].z, e[i].y) * (norm.x < 0 ? -1.0f : +1.0f);
        de[2][i] = -dot(ne[2][i], vec2(v[i].y, v[i].z)) + max(0.0f, ne[2][i].x) + max(0.0f, ne[2][i].y);
    }

    ivec3 boundMin = min(min(v[0], v[1]), v[2]);
    ivec3 boundMax = max(max(v[0], v[1]), v[2]);

    // TODO: Test specialization (iterate only over 2D plane for dominant axis)
    for (int32_t y = boundMin.y; y <= boundMax.y; y++) {
        for (int32_t z = boundMin.z; z <= boundMax.z; z++) {
            for (int32_t x = boundMin.x; x <= boundMax.x; x++) {
                vec3 p = { x, y, z };

                // Triangle plane overlap check
                float NdotP = dot(norm, p);
                if ((NdotP + d1) * (NdotP + d2) > 0.0f) continue;

                // 2D projection overlap checks
                bool overlaps = true;

                for (uint32 i = 0; i < 3 && overlaps; i++) {
                    overlaps &= dot(ne[0][i], vec2(p.x, p.y)) + de[0][i] >= 0.0f;
                    overlaps &= dot(ne[1][i], vec2(p.z, p.x)) + de[1][i] >= 0.0f;
                    overlaps &= dot(ne[2][i], vec2(p.y, p.z)) + de[2][i] >= 0.0f;
                }

                if (overlaps) visitor({ x, y, z });
            }
        }
    }
}

// Project a 3D point onto a triangle, returning barycentric coordinates.
// https://math.stackexchange.com/a/2579920
static glm::vec3 ProjectPointOnTriangle(const glm::vec3& p, const glm::vec3 vtx[3]) {
    glm::vec3 u = vtx[1] - vtx[0];
    glm::vec3 v = vtx[2] - vtx[0];
    glm::vec3 n = glm::cross(u, v);
    glm::vec3 w = p - vtx[0];
    // Barycentric coordinates of the projection P′of P onto T:
    // γ=[(u×w)⋅n]/n²
    float gamma = glm::dot(glm::cross(u, w), n) / glm::dot(n, n);
    // β=[(w×v)⋅n]/n²
    float beta = glm::dot(glm::cross(w, v), n) / glm::dot(n, n);
    float alpha = 1 - gamma - beta;

    return { alpha, beta, gamma };
}

static fastgltf::Asset LoadAsset(std::filesystem::path path) {
    constexpr auto supportedExtensions =
        fastgltf::Extensions::KHR_mesh_quantization |
        fastgltf::Extensions::KHR_texture_transform |
        fastgltf::Extensions::KHR_materials_variants;

    fastgltf::Parser parser(supportedExtensions);

    constexpr auto gltfOptions =
        fastgltf::Options::DontRequireValidAssetMember |
        fastgltf::Options::LoadExternalBuffers |
        fastgltf::Options::LoadExternalImages |
        fastgltf::Options::GenerateMeshIndices;

    auto gltfFile = fastgltf::MappedGltfFile::FromPath(path);
    if (!gltfFile) {
        throw std::runtime_error("Failed to open glTF file: " + std::string(fastgltf::getErrorMessage(gltfFile.error())));
    }

    auto asset = parser.loadGltf(gltfFile.get(), path.parent_path(), gltfOptions);
    if (asset.error() != fastgltf::Error::None) {
        throw std::runtime_error("Failed to load glTF: " + std::string(fastgltf::getErrorMessage(asset.error())));
    }

    return std::move(asset.get());
}

static swr::StbImage LoadImage(const fastgltf::Asset& asset, const fastgltf::DataSource& data) {
    if (auto uri = std::get_if<fastgltf::sources::URI>(&data)) {
        return swr::StbImage::Load(uri->uri.fspath().string());
    }
    if (auto view = std::get_if<fastgltf::sources::BufferView>(&data)) {
        auto& bufferView = asset.bufferViews[view->bufferViewIndex];
        auto& array = std::get<fastgltf::sources::Array>(asset.buffers[bufferView.bufferIndex].data);
        return swr::StbImage::LoadFromMemory((uint8_t*)&array.bytes[bufferView.byteOffset], bufferView.byteLength);
    }
    if (auto array = std::get_if<fastgltf::sources::Array>(&data)) {
        return swr::StbImage::LoadFromMemory((uint8_t*)array->bytes.data(), array->bytes.size_bytes());
    }
    throw std::runtime_error("Don't know how to handle given GLTF data source");
}

struct Vertex {
    glm::vec3 Pos;
    glm::vec2 UV;
};
static void IterateAssetPrimitives(fastgltf::Asset& asset, uint32_t sceneIndex, auto callback) {
    fastgltf::iterateSceneNodes(asset, sceneIndex, fastgltf::math::fmat4x4(), [&](fastgltf::Node& node, auto mat) {
        if (!node.meshIndex.has_value()) return;

        glm::mat4 modelMat = glm::make_mat4(mat.data());

        auto& mesh = asset.meshes[node.meshIndex.value()];

        for (auto& prim : mesh.primitives) {
            assert(prim.type == fastgltf::PrimitiveType::Triangles);

            auto& positions = asset.accessors[prim.findAttribute("POSITION")->accessorIndex];
            auto& indices = asset.accessors[prim.indicesAccessor.value()];

            auto texCoordAttr = prim.findAttribute("TEXCOORD_0");
            auto texCoords = texCoordAttr ? &asset.accessors[texCoordAttr->accessorIndex] : nullptr;
            if (texCoords->type != fastgltf::AccessorType::Vec2) texCoords = nullptr;

            Vertex verts[3];

            for (uint32_t i = 0; i < indices.count; i += 3) {
                for (uint32_t j = 0; j < 3; j++) {
                    uint32_t index = fastgltf::getAccessorElement<uint32_t>(asset, indices, i + j);

                    auto pos = fastgltf::getAccessorElement<fastgltf::math::fvec3>(asset, positions, index);
                    auto uv = texCoords ? fastgltf::getAccessorElement<fastgltf::math::fvec2>(asset, *texCoords, index) : fastgltf::math::fvec2();

                    verts[j].Pos = modelMat * glm::vec4(glm::make_vec3(pos.data()), 1.0);
                    verts[j].UV = glm::make_vec2(uv.data());
                }
                callback(verts, prim);
            }
        }
    });
}

static void AddColorsToPalette(glim::PaletteBuilder& palette, swr::RgbaTexture2D& tex) {
    if (tex.Width <= 4 || tex.Height <= 4) return;

    const auto processTile = [&](uint32_t x, uint32_t y, VFloat u, VFloat v) {
        constexpr swr::SamplerDesc SD = { .MinFilter = swr::FilterMode::Nearest, .EnableMips = true };
        auto colors = tex.Sample<SD>(u, v, 0, 2);

        for (uint32_t i = 0; i < simd::VectorWidth; i++) {
            if ((colors[i] >> 24 & 255) < 200) continue;  // skip transparent pixels
            palette.AddColor((uint32_t)colors[i]);
        }
    };
    swr::texutil::IterateTiles(tex.Width / 4, tex.Height / 4, processTile);
}

void VoxelMap::VoxelizeModel(std::string_view modelPath, glm::uvec3 startPos, glm::uvec3 size) {
    auto asset = LoadAsset(modelPath);

    // Load textures and build palette
    std::unordered_map<size_t, swr::RgbaTexture2D> textures;
    swr::RgbaTexture2D emptyTex(4, 4, 1, 1);
    std::vector<swr::RgbaTexture2D*> materialTextures;
    glim::PaletteBuilder palette;

    for (auto& mat : asset.materials) {
        if (mat.pbrData.baseColorTexture.has_value()) {
            auto& textureInfo = asset.textures[mat.pbrData.baseColorTexture->textureIndex];
            size_t imageIndex = textureInfo.imageIndex.value();

            if (textures.contains(imageIndex)) continue;

            auto image = LoadImage(asset, asset.images[imageIndex].data);

            swr::RgbaTexture2D tex(image.Width, image.Height, 4, 1);
            tex.SetPixels(image.Data.get(), image.Width, 0);
            tex.GenerateMips();

            AddColorsToPalette(palette, tex);
        
            auto slot = textures.insert({ imageIndex, std::move(tex) });
            materialTextures.push_back(&slot.first->second);
        } else {
            materialTextures.push_back(&emptyTex);
        }
    }

    palette.Build(240); // Can go up to 255, but we'll reserve a few slots for debug materials

    for (uint32_t i = 0; i < palette.NumColors; i++) {
        glm::vec3 color = glm::vec3(palette.ColorR[i], palette.ColorG[i], palette.ColorB[i]);
        Palette[i] = Material{ .Color = { palette.ColorR[i], palette.ColorG[i], palette.ColorB[i] } };
    }

    // Find scene bounding box
    glm::vec3 bounds[2] = { glm::vec3(FLT_MAX), glm::vec3(FLT_MIN) };
    size_t currentPrimitive = 0, totalPrimitives = 0;

    IterateAssetPrimitives(asset, 0, [&](Vertex verts[3], fastgltf::Primitive& prim) {
        for (uint32_t j = 0; j < 3; j++) {
            bounds[0] = glm::min(bounds[0], verts[j].Pos);
            bounds[1] = glm::max(bounds[1], verts[j].Pos);
        }
        totalPrimitives++;
    });

    glm::vec3 boundRange = bounds[1] - bounds[0];
    glm::vec3 scale = glm::vec3(size) / glm::max(glm::max(boundRange.x, boundRange.y), boundRange.z);
    glm::vec3 center = glm::vec3(startPos) + (glm::vec3(size) - (boundRange * scale)) * 0.5f;
    center.y = 0;

    // Voxelize
    IterateAssetPrimitives(asset, 0, [&](Vertex verts[3], fastgltf::Primitive& prim) {
        glm::vec3 pos[3];
        glm::vec3 texU, texV;
        swr::RgbaTexture2D* texture = materialTextures[prim.materialIndex.value()];

        for (uint32_t j = 0; j < 3; j++) {
            pos[j] = (verts[j].Pos - bounds[0]) * scale + center;
            texU[(int)j] = verts[j].UV.x;
            texV[(int)j] = verts[j].UV.y;
        }

        VoxelizeTriangleSurface(pos, [&](glm::ivec3 voxelPos) {
            auto bary = ProjectPointOnTriangle(glm::vec3(voxelPos), pos);
            float u = glm::dot(texU, bary);
            float v = glm::dot(texV, bary);

            constexpr swr::SamplerDesc SD = { .MinFilter = swr::FilterMode::Nearest, .EnableMips = true };
            auto colors = texture->Sample<SD>(u, v, 0, 1);
            if (colors[0] < 0x80'000000) return;  // alpha test

            uint32_t paletteIdx = palette.FindIndex((uint32_t)colors[0]);
            Set(voxelPos, Voxel::Create(paletteIdx));
        });

        if (currentPrimitive++ % 4096 == 0) {
            printf("Voxelizing... %llu%%\r\n", currentPrimitive * 100ull / totalPrimitives);
            fflush(stdout);
        }
    });
}