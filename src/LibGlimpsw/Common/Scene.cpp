#include "Scene.h"

#include <unordered_map>
#include <filesystem>

#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>

namespace glim {

static void PackNorm(int8_t* dst, float* src) {
    for (uint32_t i = 0; i < 3; i++) {
        dst[i] = (int8_t)std::round(src[i] * 127.0f);
    }
}
static std::string GetTextureName(const aiMaterial* mat, aiTextureType type) {
    if (mat->GetTextureCount(type) <= 0) {
        return "";
    }
    aiString path;
    mat->GetTexture(type, 0, &path);
    return std::string(path.data, path.length);
}

static void CombineNormalMR(swr::StbImage& normalMap, const swr::StbImage& mrMap) {
    uint32_t count = normalMap.Width * normalMap.Height * 4;
    uint8_t* pixels = normalMap.Data.get();
    uint8_t* mrPixels = mrMap.Data.get();

    for (uint32_t i = 0; i < count; i += 4) {
        // Re-normalize to get rid of JPEG artifacts
        glm::vec3 N = glm::vec3(pixels[i + 0], pixels[i + 1], pixels[i + 2]);
        N = glm::normalize(N / 127.0f - 1.0f) * 127.0f + 127.0f;

        pixels[i + 0] = (uint8_t)roundf(N.x);
        pixels[i + 1] = (uint8_t)roundf(N.y);

        // Overwrite BA channels from normal map with Metallic and Roughness.
        // The normal Z can be reconstructed with `sqrt(1.0f - dot(n.xy, n.xy))`
        if (mrPixels != nullptr) {
            pixels[i + 2] = mrPixels[i + 2];  // Metallic
            pixels[i + 3] = mrPixels[i + 1];  // Roughness
        }
    }
}

static void InsertEmissiveMask(swr::StbImage& baseMap, const swr::StbImage& emissiveMap) {
    uint32_t count = baseMap.Width * baseMap.Height * 4;
    uint8_t* pixels = baseMap.Data.get();
    uint8_t* emissivePixels = emissiveMap.Data.get();

    for (uint32_t i = 0; i < count; i += 4) {
        const uint8_t L = 8;
        bool isLit = emissivePixels[i + 0] > L || emissivePixels[i + 1] > L || emissivePixels[i + 2] > L;
        pixels[i + 3] = isLit ? 255 : std::min(pixels[i + 3], (uint8_t)254);
    }
}

static swr::StbImage LoadImage(Model& m, std::string_view name) {
    auto fullPath = std::filesystem::path(m.BasePath) / name;

    if (name.empty() || !std::filesystem::exists(fullPath)) {
        return { };
    }
    return swr::StbImage::Load(fullPath.string());
}

static swr::RgbaTexture2D* LoadTextures(Model& m, const aiMaterial* mat) {
    std::string name = GetTextureName(mat, aiTextureType_BASE_COLOR);

    auto cached = m.Textures.find(name);

    if (cached != m.Textures.end()) {
        return &cached->second;
    }
    if (name.empty()) {
        auto slot = m.Textures.insert({ name, swr::RgbaTexture2D(4, 4, 1, 1) });
        return &slot.first->second;
    }
    swr::StbImage baseColorImg = LoadImage(m, name);

    if (!baseColorImg.Width) {
        auto slot = m.Textures.insert({ name, swr::RgbaTexture2D(4, 4, 1, 1) });
        return &slot.first->second;
    }
    swr::StbImage normalImg = LoadImage(m, GetTextureName(mat, aiTextureType_NORMALS));
    swr::StbImage metalRoughImg = LoadImage(m, GetTextureName(mat, aiTextureType_DIFFUSE_ROUGHNESS));
    swr::StbImage emissiveImg = LoadImage(m, GetTextureName(mat, aiTextureType_EMISSIVE));

    bool hasNormals = normalImg.Width == baseColorImg.Width && normalImg.Height == baseColorImg.Height;
    bool hasEmissive = emissiveImg.Width == baseColorImg.Width && emissiveImg.Height == baseColorImg.Height;

    uint32_t numLayers = hasEmissive ? 3 : (hasNormals ? 2 : 1);
    swr::RgbaTexture2D tex(baseColorImg.Width, baseColorImg.Height, 8, numLayers);

    if (hasNormals) {
        CombineNormalMR(normalImg, metalRoughImg);
        tex.SetPixels(normalImg.Data.get(), normalImg.Width, 1);
    }
    if (hasEmissive) {
        InsertEmissiveMask(baseColorImg, emissiveImg);
        tex.SetPixels(emissiveImg.Data.get(), emissiveImg.Width, 2);
    }
    tex.SetPixels(baseColorImg.Data.get(), baseColorImg.Width, 0);
    tex.GenerateMips();

    auto slot = m.Textures.insert({ name, std::move(tex) });
    return &slot.first->second;
}

ModelNode ConvertNode(const Model& model, aiNode* node) {
    //TODO: figure out wtf is going on with empty nodes
    //FIXME: apply transform on node AABBs
    ModelNode cn = {
        .Transform = glm::transpose(*(glm::mat4*)&node->mTransformation),
        .Bounds = { glm::vec3(INFINITY), glm::vec3(-INFINITY) }
    };

    for (uint32_t i = 0; i < node->mNumMeshes; i++) {
        cn.Meshes.push_back(node->mMeshes[i]);

        const Mesh& mesh = model.Meshes[node->mMeshes[i]];
        cn.Bounds[0] = glm::min(cn.Bounds[0], mesh.Bounds[0]);
        cn.Bounds[1] = glm::max(cn.Bounds[1], mesh.Bounds[1]);
    }
    for (uint32_t i = 0; i < node->mNumChildren; i++) {
        ModelNode childNode = ConvertNode(model, node->mChildren[i]);

        cn.Bounds[0] = glm::min(cn.Bounds[0], childNode.Bounds[0]);
        cn.Bounds[1] = glm::max(cn.Bounds[1], childNode.Bounds[1]);

        cn.Children.emplace_back(std::move(childNode));
    }
    return cn;
}

Model::Model(std::string_view path) {
    const auto processFlags = aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_CalcTangentSpace | 
                              aiProcess_JoinIdenticalVertices | aiProcess_FlipUVs | aiProcess_SplitLargeMeshes |
                              aiProcess_OptimizeGraph;// | aiProcess_OptimizeMeshes;

    Assimp::Importer imp;

    if (sizeof(VertexIndex) < 4) {
        imp.SetPropertyInteger(AI_CONFIG_PP_SLM_VERTEX_LIMIT, (1 << (sizeof(VertexIndex) * 8)) - 1);
    }

    const aiScene* scene = imp.ReadFile(path.data(), processFlags);

    if (!scene || !scene->HasMeshes()) {
        throw std::exception("Could not import scene");
    }

    BasePath = std::filesystem::path(path).parent_path().string();

    for (int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial* mat = scene->mMaterials[i];

        Materials.push_back(Material{
            .Texture = LoadTextures(*this, mat),
        });
    }

    uint32_t numVertices = 0;
    uint32_t numIndices = 0;

    for (uint32_t i = 0; i < scene->mNumMeshes; i++) {
        auto mesh = scene->mMeshes[i];
        numVertices += mesh->mNumVertices;

        for (int j = 0; j < mesh->mNumFaces; j++) {
            numIndices += mesh->mFaces[j].mNumIndices;
        }
    }

    VertexBuffer = std::make_unique<Vertex[]>(numVertices);
    IndexBuffer = std::make_unique<VertexIndex[]>(numIndices);

    uint32_t vertexPos = 0, indexPos = 0;

    for (uint32_t i = 0; i < scene->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[i];

        Mesh& impMesh = Meshes.emplace_back(Mesh{
            .VertexOffset = vertexPos,
            .IndexOffset = indexPos,
            .Material = &Materials[mesh->mMaterialIndex],
            .Bounds = { glm::vec3(INFINITY), glm::vec3(-INFINITY) },
        });

        for (uint32_t j = 0; j < mesh->mNumVertices; j++) {
            Vertex& v = VertexBuffer[vertexPos++];

            glm::vec3 pos = *(glm::vec3*)&mesh->mVertices[j];
            *(glm::vec3*)&v.x = pos;

            if (mesh->HasTextureCoords(0)) {
                *(glm::vec2*)&v.u = *(glm::vec2*)&mesh->mTextureCoords[0][j];
            }
            if (mesh->HasNormals() && mesh->HasTangentsAndBitangents()) {
                PackNorm(&v.nx, &mesh->mNormals[j].x);
                PackNorm(&v.tx, &mesh->mTangents[j].x);
            }

            impMesh.Bounds[0] = glm::min(impMesh.Bounds[0], pos);
            impMesh.Bounds[1] = glm::max(impMesh.Bounds[1], pos);
        }

        for (uint32_t j = 0; j < mesh->mNumFaces; j++) {
            aiFace& face = mesh->mFaces[j];

            for (uint32_t k = 0; k < face.mNumIndices; k++) {
                IndexBuffer[indexPos++] = (VertexIndex)face.mIndices[k];
            }
        }
        impMesh.IndexCount = indexPos - impMesh.IndexOffset;
    }

    RootNode = ConvertNode(*this, scene->mRootNode);
}

}; // namespace glim