#pragma once

#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>

#include <SwRast/Rasterizer.h>
#include <SwRast/Texture.h>

namespace scene {

struct Material {
    // Layer 0: BaseColor
    // Layer 1?: Normal (XY), Metallic (Z), Roughness (W)
    // Layer 2?: Emissive, BaseColor.A==255 is a mask for non-zero emission. Normal values range between [0..254]
    const swr::RgbaTexture2D* Texture;
};

struct Mesh {
    uint32_t VertexOffset, IndexOffset, IndexCount;
    Material* Material;
    glm::vec3 Bounds[2];
};


struct Node {
    std::vector<Node> Children;
    std::vector<uint32_t> Meshes;
    glm::mat4 Transform;
    glm::vec3 Bounds[2];
};

struct Vertex {
    float x, y, z;
    float u, v;
    int8_t nx, ny, nz;
    int8_t tx, ty, tz;
};
using Index = uint16_t;

class Model {
public:

    std::string BasePath;

    std::vector<Mesh> Meshes;
    std::vector<Material> Materials;
    std::unordered_map<std::string, swr::RgbaTexture2D> Textures;

    std::unique_ptr<Vertex[]> VertexBuffer;
    std::unique_ptr<Index[]> IndexBuffer;

    Node RootNode;

    Model(std::string_view path);

    void Traverse(
        std::function<bool(const Node&, const glm::mat4&)> visitor, 
        const glm::mat4& _parentMat = glm::mat4(1.0f), const Node* _node = nullptr
    ) const {
        if (_node == nullptr) {
            _node = &RootNode;
        }

        glm::mat4 localMat = _node->Transform * _parentMat;

        if (_node->Meshes.size() > 0 && !visitor(*_node, localMat)) {
            return;
        }
        for (const Node& child : _node->Children) {
            Traverse(visitor, localMat, &child);
        }
    }
};

};  // namespace scene