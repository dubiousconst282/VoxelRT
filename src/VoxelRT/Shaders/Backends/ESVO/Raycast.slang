/*
 *  Copyright (c) 2009-2011, NVIDIA Corporation
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *      * Neither the name of NVIDIA Corporation nor the
 *        names of its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

__generic<T>
int64_t operator-(Ptr<T> a, Ptr<T> b) {
    return (int64_t(a) - int64_t(b)) / sizeof(T);
}
#include "../PerfCounters.h.slang"

//------------------------------------------------------------------------

#define CAST_STACK_DEPTH        23
#define MAX_RAYCAST_ITERATIONS  1000

#define updateCounter(...)
#define updateCountersForGlobalAccess(...)
#define updateCountersForLocalAccess(...)

public struct Ray
{
    public float3      orig;
    public float       orig_sz;
    public float3      dir;
    public float       dir_sz;
};
public struct CastResult
{
    public float       t;
    public float3      pos;
    public int         iter;
    public float3      normal;

    public uint*       node;
    public int         childIdx;
    public int         stackPtr;
};

public CastResult CastRay(Ray ray, uint* rootNode)
{
    const float epsilon = exp2(-CAST_STACK_DEPTH);
    int iter = 0;
    
    uint2 stack[CAST_STACK_DEPTH + 1];

    // Get rid of small ray direction components to avoid division by zero.

    if (abs(ray.dir.x) < epsilon) ray.dir.x = copysign(epsilon, ray.dir.x);
    if (abs(ray.dir.y) < epsilon) ray.dir.y = copysign(epsilon, ray.dir.y);
    if (abs(ray.dir.z) < epsilon) ray.dir.z = copysign(epsilon, ray.dir.z);

    // Precompute the coefficients of tx(x), ty(y), and tz(z).
    // The octree is assumed to reside at coordinates [1, 2].

    float tx_coef = 1.0f / -abs(ray.dir.x);
    float ty_coef = 1.0f / -abs(ray.dir.y);
    float tz_coef = 1.0f / -abs(ray.dir.z);

    float tx_bias = tx_coef * ray.orig.x;
    float ty_bias = ty_coef * ray.orig.y;
    float tz_bias = tz_coef * ray.orig.z;

    // Select octant mask to mirror the coordinate system so
    // that ray direction is negative along each axis.

    int octant_mask = 7;
    if (ray.dir.x > 0.0f) octant_mask ^= 1, tx_bias = 3.0f * tx_coef - tx_bias;
    if (ray.dir.y > 0.0f) octant_mask ^= 2, ty_bias = 3.0f * ty_coef - ty_bias;
    if (ray.dir.z > 0.0f) octant_mask ^= 4, tz_bias = 3.0f * tz_coef - tz_bias;

    // Initialize the active span of t-values.

    float t_min = max(max(2.0f * tx_coef - tx_bias, 2.0f * ty_coef - ty_bias), 2.0f * tz_coef - tz_bias);
    float t_max = min(min(tx_coef - tx_bias, ty_coef - ty_bias), tz_coef - tz_bias);
    float h = t_max;
    t_min = max(t_min, 0.0f);
    t_max = min(t_max, 1.0f);

    // Initialize the current voxel to the first child of the root.
    uint*  parent           = rootNode;
    int    child_descriptor = 0; // invalid until fetched
    int    idx              = 0;
    float3 pos              = 1.0f;
    int    scale            = CAST_STACK_DEPTH - 1;
    float  scale_exp2       = 0.5f; // exp2f(scale - s_max)

    if (1.5f * tx_coef - tx_bias > t_min) idx ^= 1, pos.x = 1.5f;
    if (1.5f * ty_coef - ty_bias > t_min) idx ^= 2, pos.y = 1.5f;
    if (1.5f * tz_coef - tz_bias > t_min) idx ^= 4, pos.z = 1.5f;

    // Traverse voxels along the ray as long as the current voxel
    // stays within the octree.

    while (scale < CAST_STACK_DEPTH)
    {
        updateCounter(PerfCounter_Iterations);
        updateCounter(PerfCounter_Instructions, 15 - 1);

        if (iter++ > MAX_RAYCAST_ITERATIONS)
            break;

        // Fetch child descriptor unless it is already valid.

        if (child_descriptor == 0)
        {
            child_descriptor = *parent;
            updateCountersForGlobalAccess(3, parent);
        }

        // Determine maximum t-value of the cube by evaluating
        // tx(), ty(), and tz() at its corner.

        float tx_corner = pos.x * tx_coef - tx_bias;
        float ty_corner = pos.y * ty_coef - ty_bias;
        float tz_corner = pos.z * tz_coef - tz_bias;
        float tc_max = min(min(tx_corner, ty_corner), tz_corner);

        // Process voxel if the corresponding bit in valid mask is set
        // and the active t-span is non-empty.

        int child_shift = idx ^ octant_mask; // permute child slots based on the mirroring
        int child_masks = child_descriptor << child_shift;
        if ((child_masks & 0x8000) != 0 && t_min <= t_max)
        {
            // Terminate if the voxel is small enough.

            updateCounter(PerfCounter_Instructions, 3);
            if (tc_max * ray.dir_sz + ray.orig_sz >= scale_exp2)
                break; // at t_min

            // INTERSECT
            // Intersect active t-span with the cube and evaluate
            // tx(), ty(), and tz() at the center of the voxel.

            updateCounter(PerfCounter_Intersect);
            updateCounter(PerfCounter_Instructions, 9 - 1);
            float tv_max = min(t_max, tc_max);
            float half = scale_exp2 * 0.5f;
            float tx_center = half * tx_coef + tx_corner;
            float ty_center = half * ty_coef + ty_corner;
            float tz_center = half * tz_coef + tz_corner;

            // Descend to the first child if the resulting t-span is non-empty.

            updateCounter(PerfCounter_Instructions, 2);
            if (t_min <= tv_max)
            {
                // Terminate if the corresponding bit in the non-leaf mask is not set.

                updateCounter(PerfCounter_Instructions, 2);
                if ((child_masks & 0x0080) == 0)
                    break; // at t_min (overridden with tv_min).

                // PUSH
                // Write current parent to the stack.

                updateCounter(PerfCounter_Push);
                updateCounter(PerfCounter_Instructions, 31 - 6);
#ifndef DISABLE_PUSH_OPTIMIZATION
                if (tc_max < h)
#endif
                {
                    updateCounter(PerfCounter_PushStore);
                    stack[scale] = uint2(uint(rootNode - parent), bit_cast<uint>(t_max));
                    updateCountersForLocalAccess(3, scale);
                }
                h = tc_max;

                // Find child descriptor corresponding to the current voxel.

                int ofs = (uint)child_descriptor >> 16; // child pointer
                if (ofs >= 0xFF00) // far
                {
                    ofs = parent[-(ofs - 0xFF00)];
                }
                parent = parent - ofs + countbits(child_masks & 0x7F);

                // Select child voxel that the ray enters first.

                idx = 0;
                scale--;
                scale_exp2 = half;

                if (tx_center > t_min) idx ^= 1, pos.x += scale_exp2;
                if (ty_center > t_min) idx ^= 2, pos.y += scale_exp2;
                if (tz_center > t_min) idx ^= 4, pos.z += scale_exp2;

                // Update active t-span and invalidate cached child descriptor.

                t_max = tv_max;
                child_descriptor = 0;
                continue;
            }
        }

        // ADVANCE
        // Step along the ray.

        updateCounter(PerfCounter_Advance);
        updateCounter(PerfCounter_Instructions, 14 - 2);
        int step_mask = 0;
        if (tx_corner <= tc_max) step_mask ^= 1, pos.x -= scale_exp2;
        if (ty_corner <= tc_max) step_mask ^= 2, pos.y -= scale_exp2;
        if (tz_corner <= tc_max) step_mask ^= 4, pos.z -= scale_exp2;

        // Update active t-span and flip bits of the child slot index.

        t_min = tc_max;
        idx ^= step_mask;

        // Proceed with pop if the bit flips disagree with the ray direction.

        if ((idx & step_mask) != 0)
        {
            // POP
            // Find the highest differing bit between the two positions.

            updateCounter(PerfCounter_Pop);
            updateCounter(PerfCounter_Instructions, 38 - 4);
            uint differing_bits = 0;
            if ((step_mask & 1) != 0) differing_bits |= bit_cast<uint>(pos.x) ^ bit_cast<uint>(pos.x + scale_exp2);
            if ((step_mask & 2) != 0) differing_bits |= bit_cast<uint>(pos.y) ^ bit_cast<uint>(pos.y + scale_exp2);
            if ((step_mask & 4) != 0) differing_bits |= bit_cast<uint>(pos.z) ^ bit_cast<uint>(pos.z + scale_exp2);
            scale = (bit_cast<int>((float)differing_bits) >> 23) - 127; // position of the highest bit
            scale_exp2 = bit_cast<float>((scale - CAST_STACK_DEPTH + 127) << 23); // exp2f(scale - s_max)

            // Restore parent voxel from the stack.

            uint2 stackEntry = stack[scale];
            parent = rootNode - stackEntry.x;
            t_max = bit_cast<float>(stackEntry.y);
            updateCountersForLocalAccess(3, scale);

            // Round cube position and extract child slot index.

            int shx = bit_cast<int>(pos.x) >> scale;
            int shy = bit_cast<int>(pos.y) >> scale;
            int shz = bit_cast<int>(pos.z) >> scale;
            pos.x = bit_cast<float>(shx << scale);
            pos.y = bit_cast<float>(shy << scale);
            pos.z = bit_cast<float>(shz << scale);
            idx  = (shx & 1) | ((shy & 1) << 1) | ((shz & 1) << 2);

            // Prevent same parent from being stored again and invalidate cached child descriptor.

            h = 0.0f;
            child_descriptor = 0;
        }
        updateCounter(PerfCounter_Instructions, 2);
    }
    PERF_STAT_INC(TraversalIters, iter);

    // Indicate miss if we are outside the octree.

#if (MAX_RAYCAST_ITERATIONS > 0)
    if (scale >= CAST_STACK_DEPTH || iter > MAX_RAYCAST_ITERATIONS)
#else
    if (scale >= CAST_STACK_DEPTH)
#endif
    {
        t_min = 2.0f;
    }


    // Compute normals (from https://github.com/AdamYuan/SparseVoxelOctree)
    float tx_corner = (pos.x + scale_exp2) * tx_coef - tx_bias;
    float ty_corner = (pos.y + scale_exp2) * ty_coef - ty_bias;
    float tz_corner = (pos.z + scale_exp2) * tz_coef - tz_bias;
	float3 norm = (tx_corner > ty_corner && tx_corner > tz_corner) ? float3(1, 0, 0) :
	              (ty_corner > tz_corner                           ? float3(0, 1, 0) : 
                                                                     float3(0, 0, 1));
	if ((octant_mask & 1) == 0) norm.x = -norm.x;
	if ((octant_mask & 2) == 0) norm.y = -norm.y;
	if ((octant_mask & 4) == 0) norm.z = -norm.z;

    // Undo mirroring of the coordinate system.

    if ((octant_mask & 1) == 0) pos.x = 3.0f - scale_exp2 - pos.x;
    if ((octant_mask & 2) == 0) pos.y = 3.0f - scale_exp2 - pos.y;
    if ((octant_mask & 4) == 0) pos.z = 3.0f - scale_exp2 - pos.z;

    // Output results.

    CastResult res;
    res.t = t_min;
    res.iter = iter;
    res.pos.x = ray.orig.x + t_min * ray.dir.x;
    res.pos.y = ray.orig.y + t_min * ray.dir.y;
    res.pos.z = ray.orig.z + t_min * ray.dir.z;
    res.normal = norm;
    res.node = parent;
    res.childIdx = idx ^ octant_mask ^ 7;
    res.stackPtr = scale;
    return res;
}