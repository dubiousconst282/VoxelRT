#pragma once

#include <bvh/v2/bvh.h>
#include <bvh/v2/default_builder.h>

#include <glm/vec3.hpp>

using BNode = bvh::v2::Node<float, 3, 32>;
using BBox = bvh::v2::BBox<float, 3>;
using BVec = bvh::v2::Vec<float, 3>;
using BvhBuilder = bvh::v2::DefaultBuilder<BNode>;
using Bvh = bvh::v2::Bvh<BNode>;

struct CwbvhNode {
    glm::vec3 pos;
    uint8_t exp[3];
    uint8_t imask;

    uint32_t base_index_child;
    uint32_t base_index_triangle;

    uint8_t meta[8];

    uint8_t quantized_min_x[8], quantized_max_x[8];
    uint8_t quantized_min_y[8], quantized_max_y[8];
    uint8_t quantized_min_z[8], quantized_max_z[8];
};

struct BVH8Converter {
    std::vector<CwbvhNode> bvh8_nodes;
    std::vector<uint32_t> bvh8_indices;
    const Bvh& bvh2;

    BVH8Converter(const Bvh& bvh2) : bvh2(bvh2) { bvh8_nodes.reserve(bvh2.prim_ids.size()); }

    void convert();

private:
    struct Decision {
        enum struct Type : char { LEAF, INTERNAL, DISTRIBUTE } type;

        char distribute_left;
        char distribute_right;

        float cost;
    };

    std::vector<Decision> decisions;

    int calculate_cost(int node_index);

    void get_children(int node_index, int children[8], int& child_count, int i);
    void order_children(int node_index, int children[8], int child_count);

    int count_primitives(int node_index);

    void collapse(int node_index_bvh8, int node_index_bvh2);
};