#include "BVHCommon.h"
#include <cfloat>

#pragma clang diagnostic ignored "-Wsign-conversion"

// https://github.com/jan-van-bergen/GPU-Raytracer/blob/master/Src/BVH/Converters/BVH8Converter.cpp
/*
 * MIT License
 *
 * Copyright (c) 2021-2022 Jan van Bergen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

void BVH8Converter::convert() {
	bvh8_indices.clear();
	bvh8_indices.reserve(bvh2.prim_ids.size());

	bvh8_nodes.clear();
	bvh8_nodes.emplace_back(); // Root

	decisions.resize(bvh2.nodes.size() * 7);

	// Fill cost table using dynamic programming (bottom up)
	calculate_cost(0);

	// Collapse SBVH into 8-way tree (top down)
	collapse(0, 0);
	assert(bvh8_indices.size() == bvh2.prim_ids.size());
}

int BVH8Converter::calculate_cost(int node_index) {
	const BNode& node = bvh2.nodes[node_index];

	int num_primitives;

	if (node.is_leaf()) {
		num_primitives = node.index.prim_count();
        assert(num_primitives == 1 && "BVH8 Builder expects BVH with leaf Nodes containing only 1 primitive!");

		// SAH cost
		float cost_leaf = node.get_bbox().get_half_area() * float(num_primitives);

		for (int i = 0; i < 7; i++) {
			decisions[node_index * 7 + i].type = Decision::Type::LEAF;
			decisions[node_index * 7 + i].cost = cost_leaf;
		}
	} else {
		num_primitives =
			calculate_cost(node.index.first_id()) +
			calculate_cost(node.index.first_id() + 1);

		// Separate case: i=0 (i=1 in the paper)
		{
			float cost_leaf = num_primitives <= 3 ? float(num_primitives) * node.get_bbox().get_half_area() : FLT_MAX;

			float cost_distribute = FLT_MAX;

			char distribute_left  = -1;
			char distribute_right = -1;

			for (int k = 0; k < 7; k++) {
				float c =
					decisions[(node.index.first_id())     * 7 +     k].cost +
					decisions[(node.index.first_id() + 1) * 7 + 6 - k].cost;

				if (c < cost_distribute) {
					cost_distribute = c;

					distribute_left  =     k;
					distribute_right = 6 - k;
				}
			}

			float cost_internal = cost_distribute + node.get_bbox().get_half_area();

			if (cost_leaf < cost_internal) {
				decisions[node_index * 7].type = Decision::Type::LEAF;
				decisions[node_index * 7].cost = cost_leaf;
			} else {
				decisions[node_index * 7].type = Decision::Type::INTERNAL;
				decisions[node_index * 7].cost = cost_internal;
			}

			decisions[node_index * 7].distribute_left  = distribute_left;
			decisions[node_index * 7].distribute_right = distribute_right;
		}

		// In the paper i=2..7
		for (int i = 1; i < 7; i++) {
			float cost_distribute = decisions[node_index * 7 + i - 1].cost;

			char distribute_left  = -1;
			char distribute_right = -1;

			for (int k = 0; k < i; k++) {
				float c =
					decisions[(node.index.first_id())     * 7 +     k    ].cost +
					decisions[(node.index.first_id() + 1) * 7 + i - k - 1].cost;

				if (c < cost_distribute) {
					cost_distribute = c;

					distribute_left  =     k;
					distribute_right = i - k - 1;
				}
			}

			decisions[node_index * 7 + i].cost = cost_distribute;

			if (distribute_left != -1) {
				decisions[node_index * 7 + i].type = Decision::Type::DISTRIBUTE;
				decisions[node_index * 7 + i].distribute_left  = distribute_left;
				decisions[node_index * 7 + i].distribute_right = distribute_right;
			} else {
				decisions[node_index * 7 + i] = decisions[node_index * 7 + i - 1];
			}
		}
	}

	return num_primitives;
}

void BVH8Converter::get_children(int node_index, int children[8], int & child_count, int i) {
	const BNode& node = bvh2.nodes[node_index];

	if (node.is_leaf()) {
		children[child_count++] = node_index;
		return;
	}

	char distribute_left  = decisions[node_index * 7 + i].distribute_left;
	char distribute_right = decisions[node_index * 7 + i].distribute_right;

	assert(distribute_left  >= 0 && distribute_left  < 7);
	assert(distribute_right >= 0 && distribute_right < 7);

	// Recurse on left child if it needs to distribute
	if (decisions[node.index.first_id() * 7 + distribute_left].type == Decision::Type::DISTRIBUTE) {
		get_children(node.index.first_id(), children, child_count, distribute_left);
	} else {
		children[child_count++] = node.index.first_id();
	}

	// Recurse on right child if it needs to distribute
	if (decisions[(node.index.first_id() + 1) * 7 + distribute_right].type == Decision::Type::DISTRIBUTE) {
		get_children(node.index.first_id() + 1, children, child_count, distribute_right);
	} else {
		children[child_count++] = node.index.first_id() + 1;
	}
}

void BVH8Converter::order_children(int node_index, int children[8], int child_count) {
	BVec p = bvh2.nodes[node_index].get_bbox().get_center();

	float cost[8][8] = { };

	// Fill cost table
	for (int c = 0; c < child_count; c++) {
		for (int s = 0; s < 8; s++) {
			BVec direction(
				(s & 0b100) ? -1.0f : +1.0f,
				(s & 0b010) ? -1.0f : +1.0f,
				(s & 0b001) ? -1.0f : +1.0f
			);

			cost[c][s] = dot(bvh2.nodes[children[c]].get_bbox().get_center() - p, direction);
		}
	}

	int   assignment[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	bool slot_filled[8] = { };

	// Greedy child ordering, the paper mentions this as an alternative
	// that works about as well as the auction algorithm in practice
	while (true) {
		float min_cost = FLT_MAX;

		int min_slot  = -1;
		int min_index = -1;

		// Find cheapest unfilled slot of any unassigned child
		for (int c = 0; c < child_count; c++) {
			if (assignment[c] == -1) {
				for (int s = 0; s < 8; s++) {
					if (!slot_filled[s] && cost[c][s] < min_cost) {
						min_cost = cost[c][s];

						min_slot  = s;
						min_index = c;
					}
				}
			}
		}

		if (min_slot == -1) break;

		slot_filled[min_slot]  = true;
		assignment [min_index] = min_slot;
	}

	// Permute children array according to assignment
	int children_copy[8] = { };
	for (int i = 0; i < 8; i++) {
		children_copy[i] = children[i];
		children[i] = -1;
	}
	for (int i = 0; i < child_count; i++) {
		assert(assignment   [i] != -1);
		assert(children_copy[i] != -1);
		children[assignment[i]] = children_copy[i];
	}
}

// Recursively count triangles in subtree of the given Node
// Simultaneously fills the indices buffer of the BVH8
int BVH8Converter::count_primitives(int node_index) {
	const BNode& node = bvh2.nodes[node_index];

	if (node.is_leaf()) {
		assert(node.index.prim_count() == 1);

		for (unsigned i = 0; i < node.index.prim_count(); i++) {
			bvh8_indices.push_back(bvh2.prim_ids[node.index.first_id() + i]);
		}

		return node.index.prim_count();
	}

	return
		count_primitives(node.index.first_id()) +
		count_primitives(node.index.first_id() + 1);
}

void BVH8Converter::collapse(int node_index_bvh8, int node_index_bvh2) {
	CwbvhNode& node = bvh8_nodes[node_index_bvh8];
	const BBox& aabb = bvh2.nodes[node_index_bvh2].get_bbox();

	memset(&node, 0, sizeof(CwbvhNode));
	node.pos = glm::vec3(aabb.min.values[0], aabb.min.values[1], aabb.min.values[2]);

	constexpr int Nq = 8;
	constexpr float denom = 1.0f / float((1 << Nq) - 1);

	glm::vec3 e(
		exp2f(ceilf(log2f((aabb.max[0] - aabb.min[0]) * denom))),
		exp2f(ceilf(log2f((aabb.max[1] - aabb.min[1]) * denom))),
		exp2f(ceilf(log2f((aabb.max[2] - aabb.min[2]) * denom)))
	);

	glm::vec3 one_over_e = 1.0f / e;

	unsigned u_ex = std::bit_cast<unsigned>(e.x);
	unsigned u_ey = std::bit_cast<unsigned>(e.y);
	unsigned u_ez = std::bit_cast<unsigned>(e.z);

	// Only the exponent bits can be non-zero
	assert((u_ex & 0b10000000011111111111111111111111) == 0);
	assert((u_ey & 0b10000000011111111111111111111111) == 0);
	assert((u_ez & 0b10000000011111111111111111111111) == 0);

	// Store only 8 bit exponent
	node.exp[0] = u_ex >> 23;
	node.exp[1] = u_ey >> 23;
	node.exp[2] = u_ez >> 23;

	int child_count = 0;
	int children[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	get_children(node_index_bvh2, children, child_count, 0);
	assert(child_count <= 8);

	order_children(node_index_bvh2, children, child_count);

	node.imask = 0;

	node.base_index_child = bvh8_nodes.size();
	node.base_index_triangle = bvh8_indices.size();

	int num_internal_nodes = 0;
	int num_triangles      = 0;

	for (int i = 0; i < 8; i++) {
		int child_index = children[i];
		if (child_index == -1) continue; // Empty slot

		const BBox& child_aabb = bvh2.nodes[child_index].get_bbox();

		node.quantized_min_x[i] = uint8_t(floorf((child_aabb.min[0] - node.pos.x) * one_over_e.x));
		node.quantized_min_y[i] = uint8_t(floorf((child_aabb.min[1] - node.pos.y) * one_over_e.y));
		node.quantized_min_z[i] = uint8_t(floorf((child_aabb.min[2] - node.pos.z) * one_over_e.z));

		node.quantized_max_x[i] = uint8_t(ceilf((child_aabb.max[0] - node.pos.x) * one_over_e.x));
		node.quantized_max_y[i] = uint8_t(ceilf((child_aabb.max[1] - node.pos.y) * one_over_e.y));
		node.quantized_max_z[i] = uint8_t(ceilf((child_aabb.max[2] - node.pos.z) * one_over_e.z));

		switch (decisions[child_index * 7].type) {
			case Decision::Type::LEAF: {
				int triangle_count = count_primitives(child_index);
				assert(triangle_count > 0 && triangle_count <= 3);

				// Three highest bits contain unary representation of triangle count
				for (int j = 0; j < triangle_count; j++) {
					node.meta[i] |= (1 << (j + 5));
				}
				node.meta[i] |= num_triangles;

				num_triangles += triangle_count;
				assert(num_triangles <= 24);
				break;
			}
			case Decision::Type::INTERNAL: {
				node.meta[i] = (i + 24) | 0b00100000;

				node.imask |= (1 << i);
				num_internal_nodes++;
				break;
			}
			default: std::unreachable();
		}
	}

	for (int i = 0; i < num_internal_nodes; i++) {
		bvh8_nodes.emplace_back();
	}
	node = bvh8_nodes[node_index_bvh8]; // NOTE: 'node' may have been invalidated by emplace_back on previous line

	assert(node.base_index_child    + num_internal_nodes == bvh8_nodes.size());
	assert(node.base_index_triangle + num_triangles      == bvh8_indices.size());

	// Recurse on Internal Nodes
	int offset = node.base_index_child;
	for (int i = 0; i < 8; i++) {
		int child_index = children[i];
		if (child_index == -1) continue;

		if (node.imask & (1 << i)) {
			collapse(offset++, child_index);
		}
	}
}