// Based on
// - https://github.com/jan-van-bergen/GPU-Raytracer/blob/master/Src/CUDA/Raytracing/BVH8.h
// - https://research.nvidia.com/sites/default/files/publications/ylitie2017hpg-paper.pdf

#include "../PerfCounters.h.slang"
import "../BackendCommon";

struct CwbvhNode {
    float4 data[5];
};

uint8_t extract_byte(uint v, uint idx) { return uint8_t(v >> (idx * 8) & 255); }

// Splat sign bit of packed bytes
//   x[i] = (x[i] & 0x80) ? 255 : 0
static uint sign_extend_s8x4(uint x) {
    // return (x >> 7 & 0x01010101) * 255;
    x = (x >> 7 & 0x01010101);
    return (x << 8) - x;
}

uint bvh8_node_intersect(
    float3 ray_origin, float3 ray_dir,
    uint oct_inv4, float max_distance, CwbvhNode node
) {
    float3 p = node.data[0].xyz;

    uint e_imask = asuint(node.data[0].w);
    uint8_t e_x = extract_byte(e_imask, 0);
    uint8_t e_y = extract_byte(e_imask, 1);
    uint8_t e_z = extract_byte(e_imask, 2);

    float3 adjusted_ray_direction_inv = asfloat(uint3(e_x, e_y, e_z) << 23) / ray_dir;
    float3 adjusted_ray_origin = (p - ray_origin) / ray_dir;

    uint hit_mask = 0;

    [unroll]
    for (int i = 0; i < 2; i++) {
        uint meta4 = asuint(i == 0 ? node.data[1].z : node.data[1].w);

        uint is_inner4 = (meta4 & (meta4 << 1)) & 0x10101010;
        uint inner_mask4 = sign_extend_s8x4(is_inner4 << 3);
        uint bit_index4 = (meta4 ^ (oct_inv4 & inner_mask4)) & 0x1f1f1f1f;
        uint child_bits4 = (meta4 >> 5) & 0x07070707;

        // Select near and far planes based on ray octant
        uint q_lo_x = asuint(i == 0 ? node.data[2].x : node.data[2].y);
        uint q_hi_x = asuint(i == 0 ? node.data[2].z : node.data[2].w);

        uint q_lo_y = asuint(i == 0 ? node.data[3].x : node.data[3].y);
        uint q_hi_y = asuint(i == 0 ? node.data[3].z : node.data[3].w);

        uint q_lo_z = asuint(i == 0 ? node.data[4].x : node.data[4].y);
        uint q_hi_z = asuint(i == 0 ? node.data[4].z : node.data[4].w);

        uint x_min = ray_dir.x < 0.0f ? q_hi_x : q_lo_x;
        uint x_max = ray_dir.x < 0.0f ? q_lo_x : q_hi_x;

        uint y_min = ray_dir.y < 0.0f ? q_hi_y : q_lo_y;
        uint y_max = ray_dir.y < 0.0f ? q_lo_y : q_hi_y;

        uint z_min = ray_dir.z < 0.0f ? q_hi_z : q_lo_z;
        uint z_max = ray_dir.z < 0.0f ? q_lo_z : q_hi_z;

        [unroll]
        for (int j = 0; j < 4; j++) {
            // Extract j-th byte
            float3 tmin3 = float3(extract_byte(x_min, j), extract_byte(y_min, j), extract_byte(z_min, j));
            float3 tmax3 = float3(extract_byte(x_max, j), extract_byte(y_max, j), extract_byte(z_max, j));

            // Account for grid origin and scale
            tmin3 = tmin3 * adjusted_ray_direction_inv + adjusted_ray_origin;
            tmax3 = tmax3 * adjusted_ray_direction_inv + adjusted_ray_origin;

            float tmin = max(max(tmin3.x, tmin3.y), max(tmin3.z, 0.0f));
            float tmax = min(min(tmax3.x, tmax3.y), min(tmax3.z, max_distance));

            bool intersected = tmin < tmax;
            if (intersected) {
                uint child_bits = extract_byte(child_bits4, j);
                uint bit_index = extract_byte(bit_index4, j);

                hit_mask |= child_bits << bit_index;
            }
        }
    }

    return hit_mask;
}

// Constants used by Dynamic Fetch Heuristic (see section 4.4 of Ylitie et al. 2017)
#define N_d 4
#define N_w 16

#define INVALID (uint)(-1)
#define BVH8_TRIANGLE_POSTPONING_THRESHOLD_DIVISOR 5

#define SHARED_STACK_INDEX(idx) \
    (GroupInvocationIndex() + idx * GROUP_SIZE)

#if 0
#define stack_push(item)                                     \
    if (stack_size < SHARED_STACK_SIZE) {                    \
        shared_stack[SHARED_STACK_INDEX(stack_size)] = item; \
    } else {                                                 \
        stack[stack_size - SHARED_STACK_SIZE] = item;        \
    }                                                        \
    stack_size++;

#define stack_pop() \
    --stack_size < SHARED_STACK_SIZE ? \
		shared_stack[SHARED_STACK_INDEX(stack_size)] : \
	    stack[stack_size - SHARED_STACK_SIZE];
#else

#define stack_push(item) stack[stack_size++] = item;
#define stack_pop() stack[--stack_size];

#endif

uint ray_get_octant_inv4(float3 ray_direction) {
    return (ray_direction.x < 0.0f ? 0 : 0x04040404) |
           (ray_direction.y < 0.0f ? 0 : 0x02020202) |
           (ray_direction.z < 0.0f ? 0 : 0x01010101);
}

float4 load4(float4* ptr) {
    return spirv_asm {
        result:$$float4 = OpLoad $ptr Aligned 16
    };
}

CwbvhNode load_node(CwbvhNode* ptr) {
    CwbvhNode node;
    // slang assumes minimal align under scalar layout
    node.data[0] = load4((float4*)ptr + 0);
    node.data[1] = load4((float4*)ptr + 1);
    node.data[2] = load4((float4*)ptr + 2);
    node.data[3] = load4((float4*)ptr + 3);
    node.data[4] = load4((float4*)ptr + 4);
    return node;
}
HitInfo bvh8_trace(CwbvhNode* node_pool, float3 ray_origin, float3 ray_dir) {
    static const uint STACK_SIZE = 32, SHARED_STACK_SIZE = 8;
    static const uint GROUP_SIZE = 64;

    static groupshared uint2 shared_stack[SHARED_STACK_SIZE * GROUP_SIZE];
    uint2 stack[STACK_SIZE - SHARED_STACK_SIZE];
    int stack_size = 0;

    uint oct_inv4 = ray_get_octant_inv4(ray_dir);
    float ray_hit_t = FLT_MAX;

    uint2 current_group = uint2(0, 0x80000000);
    uint iters = 0;

    for (; iters < 64; iters++) {
        uint2 triangle_group;

        if ((current_group.y & 0xff000000) != 0) {
            uint hits_imask = current_group.y;

            uint child_index_offset = firstbithigh(hits_imask);
            uint child_index_base = current_group.x;

            // Remove n from current_group;
            current_group.y &= ~(1 << child_index_offset);

            // If the node group is not yet empty, push it on the stack
            if ((current_group.y & 0xff000000) != 0) {
                stack_push(current_group);
            }

            uint slot_index = (child_index_offset - 24) ^ (oct_inv4 & 0xff);
            uint relative_index = countbits(hits_imask & ~(0xffffffff << slot_index));

            uint child_node_index = child_index_base + relative_index;
            CwbvhNode node = load_node(&node_pool[child_node_index]);

            uint hitmask = bvh8_node_intersect(ray_origin, ray_dir, oct_inv4, ray_hit_t, node);

            uint8_t imask = extract_byte(asuint(node.data[0].w), 3);

            current_group.x = asuint(node.data[1].x);  // Child    base offset
            triangle_group.x = asuint(node.data[1].y); // Triangle base offset

            current_group.y = (hitmask & 0xff000000) | imask;
            triangle_group.y = (hitmask & 0x00ffffff);
        } else {
            triangle_group = current_group;
            current_group = 0;
        }

        // FIXME: brick intersect
        if (triangle_group.y != 0) break;

        // While the triangle group is not empty
        while (triangle_group.y != 0) {
            int triangle_index = firstbithigh(triangle_group.y);
            triangle_group.y &= ~(1 << triangle_index);

            // triangle_intersect(mesh_id, triangle_group.x + triangle_index, ray, ray_hit);
            break;
        }

        if ((current_group.y & 0xff000000) == 0) {
            if (stack_size == 0) {
                //traversal_data->hits.set(ray_index, ray_hit);

                current_group.y = 0;

                break;
            }
            current_group = stack_pop();
        }
    }
    PERF_STAT_INC(TraversalIters, iters);
    return HitInfo();
}

static const float FLT_MAX = 3.402823466385288598117042E+38f;

// SubgroupLocalInvocationId + SubgroupId × SubgroupSize
// https://registry.khronos.org/vulkan/specs/1.3-extensions/html/chap15.html
uint GroupInvocationIndex() {
    uint subgroupId = spirv_asm {
        result : $$uint = OpLoad builtin(SubgroupId:uint);
    };
    return WaveGetLaneIndex() + subgroupId * WaveGetLaneCount();
}

struct Brick {
    uint64_t PopMask[8];
}

namespace backend::cwbvh {

public struct VoxelMap : IRayCastable {
    CwbvhNode* NodePool;
    Brick* BrickPool;
    uint unused1;

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        float3 origin = worldOrigin + rayPos;

        HitInfo hit = bvh8_trace(NodePool, origin, rayDir);

        if (hit.MaterialId != 0) {
            hit.Pos -= worldOrigin;
            hit.Dist = length(hit.Pos);
        }
        return hit;
    }
}

}