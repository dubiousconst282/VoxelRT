#include "VoxelMap.h"
#include "Common/PaletteBuilder.h"

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

void VoxelMap::VoxelizeModel(const glim::Model& model, glm::uvec3 startPos, glm::uvec3 size) {
    glim::PaletteBuilder palette;

    for (auto& [name, tex] : model.Textures) {
        if (tex.Width <= 4 || tex.Height <= 4) continue;

        const auto processTile = [&](uint32_t x, uint32_t y, swr::simd::VFloat u, swr::simd::VFloat v) {
            constexpr swr::SamplerDesc SD = { .MinFilter = swr::FilterMode::Nearest, .EnableMips = true };
            auto colors = tex.Sample<SD>(u, v, 0, 2);

            for (uint32_t i = 0; i < swr::simd::VInt::Length; i++) {
                if ((colors[i] >> 24 & 255) < 200) continue;  // skip transparent pixels
                palette.AddColor((uint32_t)colors[i]);
            }
        };
        swr::texutil::IterateTiles(tex.Width / 4, tex.Height / 4, processTile);
    }

    palette.Build(240); // Can go up to 255, but we'll reserve a few slots for debug materials

    for (uint32_t i = 0; i < palette.NumColors; i++) {
        glm::vec3 color = glm::vec3(palette.ColorR[i], palette.ColorG[i], palette.ColorB[i]);
        Palette[i] = Material::CreateDiffuse(color * (1.0f / 255));
    }

    glm::vec3 boundMin = glm::vec3(+INFINITY), boundMax = glm::vec3(-INFINITY);

    model.Traverse([&](const glim::ModelNode& node, const glm::mat4& modelMat) {
        for (size_t i = 0; i < 2; i++) {
            glm::vec3 pos = modelMat * glm::vec4(node.Bounds[i], 0.0);
            boundMin = glm::min(boundMin, pos);
            boundMax = glm::max(boundMax, pos);
        }
        return true;
    });

    glm::vec3 boundRange = boundMax - boundMin;
    glm::vec3 scale = glm::vec3(size) / glm::max(glm::max(boundRange.x, boundRange.y), boundRange.z);
    glm::vec3 center = glm::vec3(startPos) + (glm::vec3(size) - (boundRange * scale)) * 0.5f;
    center.y = 0;

    glm::vec3 verts[3];
    glm::vec3 texU, texV;

    model.Traverse([&](const glim::ModelNode& node, const glm::mat4& modelMat) {
        for (uint32_t meshId : node.Meshes) {
            const glim::Mesh& mesh = model.Meshes[meshId];

            for (uint32_t i = 0; i < mesh.IndexCount; i += 3) {
                for (uint32_t j = 0; j < 3; j++) {
                    auto& vtx = model.VertexBuffer[mesh.VertexOffset + model.IndexBuffer[mesh.IndexOffset + i + j]];
                    glm::vec3 pos = modelMat * glm::vec4(vtx.x, vtx.y, vtx.z, 0.0);
                    verts[j] = (pos - boundMin) * scale + center;
                    texU[(int32_t)j] = vtx.u;
                    texV[(int32_t)j] = vtx.v;
                }

                VoxelizeTriangleSurface(verts, [&](glm::ivec3 pos) {
                    auto bary = ProjectPointOnTriangle(glm::vec3(pos), verts);
                    float u = glm::dot(texU, bary);
                    float v = glm::dot(texV, bary);

                    constexpr swr::SamplerDesc SD = { .MinFilter = swr::FilterMode::Nearest, .EnableMips = true };
                    auto colors = mesh.Material->Texture->Sample<SD>(u, v, 0, 2);
                    if (colors[0] < 0x80'000000) return;  // alpha test

                    uint32_t paletteIdx = palette.FindIndex((uint32_t)colors[0]);
                    Set(pos, Voxel::Create(paletteIdx));
                });
            }
        }
        return true;
    });
}