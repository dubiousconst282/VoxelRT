
import "../BackendCommon";
#include "../PerfCounters.h.slang"

namespace backend::tree64 {

public struct Node {
    uint PackedData[3];

    property bool IsLeaf {
        get { return (PackedData[0] & 1) != 0; }
    }
    property uint ChildPtr {
        get { return PackedData[0] >> 1; }
    }
    property uint64_t PopMask {
        get { return PackedData[1] | uint64_t(PackedData[2]) << 32; }
    }
}

public struct VoxelMap : IRayCastable {
    public uint TreeScale;
    public Node* TreeNodes;
    public uint8_t* LeafData;

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        // Multiply world origin separately to minimize float rounding errors.
        // Compiler might mess with this, but oh well.
        float scale = 1.0 / (1 << TreeScale);
        float3 origin = worldOrigin * scale + rayPos * scale + 1.0;

        HitInfo hit = Traversal_OctMirror(origin, rayDir, coarse);
        
        if (hit.MaterialId != 0) {
            hit.Pos = (hit.Pos - 1.0 - worldOrigin * scale) * (1 << TreeScale);
        }
        return hit;
    }

    private HitInfo Traversal(float3 origin, float3 dir, bool coarse) {
        uint stack[11];
        int scaleExp = 21;  // 0.25

        uint nodeIdx = 0; // root
        Node node = TreeNodes[nodeIdx];

        if (abs(dir.x) < 0.0001) dir.x = 0.0001;
        if (abs(dir.y) < 0.0001) dir.y = 0.0001;
        if (abs(dir.z) < 0.0001) dir.z = 0.0001;

        float3 invDir = 1.0f / dir;
        float3 pos = clamp(origin, 1.0f, 1.9999999f);
        
        float3 sideDist;
        int i;

        for (i = 0; i < 256; i++) {
            if (coarse && i > 20 && node.IsLeaf) break;
            // Early out for too few lanes active doesn't help much and causes light
            // leaking or darkening, depending on whether we tell it's a hit or miss.
            //if (coarse && i > 30 && WaveActiveCountBits(true) < WaveGetLaneCount()*1/4) break;
            
            int childIdx = GetNodeCellIndex(pos, scaleExp);

            // Descend
            while ((node.PopMask >> childIdx & 1) != 0 && !node.IsLeaf && scaleExp >= 2) {
                stack[scaleExp >> 1] = nodeIdx;
                nodeIdx = node.ChildPtr + popcnt_var64(node.PopMask, childIdx);
                node = TreeNodes[nodeIdx];
                
                scaleExp -= 2;
                childIdx = GetNodeCellIndex(pos, scaleExp);
            }
            if ((node.PopMask >> childIdx & 1) != 0 && node.IsLeaf) break;

            int advScaleExp = scaleExp;

            // 2³ steps
            if ((node.PopMask >> (childIdx & 0b101010) & 0x00330033) == 0) advScaleExp++;

            // Compute next pos by intersecting with max cube edges
            pos = FloorScale(pos, advScaleExp);
            float3 prevPos = pos;
            
            float scale = bit_cast<float>((advScaleExp - 23 + 127) << 23);  // 2 ^ -(23 - advScaleExp)
            sideDist = (step(0.0f, dir) * scale + (pos - origin)) * invDir;

            // Distance biasing: corner misses
            //   tmax = max(tmax, min(min(sideDist.x, sideDist.y), sideDist.z)) + 0.000004;
            //   pos = origin + dir * tmax;
            
            // Nudging: grid-like artifacts
            //   pos += select(tmax == sideDist, copysign(scale * 0.01, dir), 0);

            // Bounds clamping: robust but slightly more expansive
            float tmax = min(min(sideDist.x, sideDist.y), sideDist.z);

            float3 siblPos0 = select(tmax == sideDist, pos + copysign(scale, dir), pos);
            float3 siblPos1 = asfloat(bit_cast<int3>(siblPos0) + ((1 << advScaleExp) - 1));
            pos = clamp(origin + dir * tmax, siblPos0, siblPos1); // clamp to sibling bounds [0, 1)

            // Find carry bit to tell which node we should ascend to
            uint3 diffPos = asuint(pos) ^ asuint(prevPos);
            int diffExp = firstbithigh(diffPos.x | diffPos.y | diffPos.z); // 31 - lzcnt
            if (diffExp % 2 == 0) diffExp--;

            if (diffExp > scaleExp) {
                if (diffExp > 21) break;  // going out of root?

                scaleExp = diffExp;
                nodeIdx = stack[scaleExp >> 1];
                node = TreeNodes[nodeIdx];
            }
        }
        PERF_STAT_INC(TraversalIters, i);
        
        HitInfo hit;
        hit.MaterialId = 0;
        
        if (node.IsLeaf) {
            int childIdx = GetNodeCellIndex(pos, scaleExp);
            hit.MaterialId = LeafData[node.ChildPtr + popcnt_var64(node.PopMask, childIdx)];
            hit.Pos = pos;

            float tmax = min(min(sideDist.x, sideDist.y), sideDist.z);
            bool3 sideMask = tmax >= sideDist;
            hit.Normal = select(sideMask, -sign(dir), 0.0);
        }
        return hit;
    }

    private HitInfo Traversal_OctMirror(float3 origin, float3 dir, bool coarse) {
        uint groupId = spirv_asm {
            OpCapability VariablePointers;
            result:$$uint = OpLoad builtin(LocalInvocationIndex:uint);
        };
        static groupshared uint gs_stack[64][11];
        //uint stack[11];
        uint* stack = &gs_stack[groupId][0];
        int scaleExp = 21;  // 0.25

        uint nodeIdx = 0; // root
        Node node = TreeNodes[nodeIdx];

        float3 invDir = 1.0f / -abs(dir);
        uint mirrorMask = 0;

        // Mirror coordinates to negative ray octant
        if (dir.x > 0) mirrorMask |= 3 << 0;
        if (dir.y > 0) mirrorMask |= 3 << 4;
        if (dir.z > 0) mirrorMask |= 3 << 2;

        origin = GetMirroredPos(origin, dir, true);
        float3 pos = clamp(origin, 1.0f, 1.9999999f);
        
        float3 sideDist;
        int childIdx;
        int i;

        for (i = 0; i < 256; i++) {
            if (coarse && i > 20 && node.IsLeaf) break;

            childIdx = GetNodeCellIndex(pos, scaleExp) ^ mirrorMask;

            // Descend
            while ((node.PopMask >> childIdx & 1) != 0 && !node.IsLeaf) {
                stack[scaleExp >> 1] = nodeIdx;
                nodeIdx = node.ChildPtr + popcnt_var64(node.PopMask, childIdx);
                node = TreeNodes[nodeIdx];
                
                scaleExp -= 2;
                childIdx = GetNodeCellIndex(pos, scaleExp) ^ mirrorMask;
            }
            if ((node.PopMask >> childIdx & 1) != 0 && node.IsLeaf) break;

            int advScaleExp = scaleExp;

            // 2³ steps
            if ((node.PopMask >> (childIdx & 0b101010) & 0x00330033) == 0) advScaleExp++;

            // Compute next pos by intersecting with max cube edges
            float3 edgePos = FloorScale(pos, advScaleExp);
            
            sideDist = (edgePos - origin) * invDir;
            float tmax = min(min(sideDist.x, sideDist.y), sideDist.z);
            
            int3 maxSiblBounds = bit_cast<int3>(edgePos) + select(sideDist == tmax, -1, (1 << advScaleExp) - 1);
            pos = min(origin - abs(dir) * tmax, asfloat(maxSiblBounds));

            // Find carry bit to tell which node we should ascend to
            uint3 diffPos = asuint(pos) ^ asuint(edgePos);
            int diffExp = firstbithigh(diffPos.x | diffPos.y | diffPos.z); // 31 - lzcnt
            if (diffExp % 2 == 0) diffExp--;

            if (diffExp > scaleExp) {
                scaleExp = diffExp;
                if (diffExp > 21) break;  // going out of root?

                nodeIdx = stack[scaleExp >> 1];
                node = TreeNodes[nodeIdx];
            }
        }
        PERF_STAT_INC(TraversalIters, i);
        
        HitInfo hit;
        hit.MaterialId = 0;
        
        if (node.IsLeaf && scaleExp <= 21) {
            pos = GetMirroredPos(pos, dir, false);

            hit.MaterialId = LeafData[node.ChildPtr + popcnt_var64(node.PopMask, childIdx)];
            hit.Pos = pos;

            float tmax = min(min(sideDist.x, sideDist.y), sideDist.z);
            bool3 sideMask = tmax >= sideDist;
            hit.Normal = select(sideMask, -sign(dir), 0.0);
        }
        return hit;
    }

    // Computes `dir > 0 ? 3.0 - pos : pos`
    // with special case to avoid rounding.
    static float3 GetMirroredPos(float3 pos, float3 dir, bool rangeCheck) {
        float3 mirrored = asfloat(asuint(pos) ^ 0x7FFFFF);
        // XOR'ing will only work if the coords are in range [1.0, 2.0),
        // so just fallback to the original subtraction if that's not the case.
        if (rangeCheck && any(pos < 1.0 || pos >= 2.0)) mirrored = 3.0 - pos;
        return select(dir > 0, mirrored, pos);
    }

    private HitInfo Traversal_HDDA(float3 origin, float3 dir, bool coarse) {
        uint stack[11];
        int scale_exp = 21;  // 0.25

        Node* node = &TreeNodes[0];
        stack[scale_exp >> 1] = 0;

        if (abs(dir.x) < 0.0001) dir.x = 0.0001;
        if (abs(dir.y) < 0.0001) dir.y = 0.0001;
        if (abs(dir.z) < 0.0001) dir.z = 0.0001;

        int signMask = 0, stepIdx = 0, currIdx = 0;
        if (dir.x < 0) signMask |= 0b11 << 0;
        if (dir.y < 0) signMask |= 0b11 << 4;
        if (dir.z < 0) signMask |= 0b11 << 2;

        float3 invDir = 1.0f / dir;
        float3 pos = FloorScale(origin, scale_exp);

        // Ensure start pos is inside the grid
        // This is not correct but cheap and prevents the traversal from completely breaking down.
        pos = clamp(pos, 1.0f, 1.9999999f);

        float tmin = 0.0f;
        int j = 0;

        for (j = 0; j < (coarse ? 80 : 128); j++) {
            float scale = bit_cast<float>((scale_exp - 23 + 127) << 23);  // 2 ^ -(23 - scale_exp)
            float3 sideDist = (step(0.0f, dir) * scale + (pos - origin)) * invDir;

            currIdx = GetNodeCellIndex(pos, scale_exp);
            uint64_t mask = node->PopMask;

            // DDA takes at worst N*3-1 iterations to traverse a N³ grid
            [loop] // this really doesn't need to be unrolled, burns too many registers for some reason
            for (int i = 0; i < 11; i++) {
                if ((mask >> currIdx & 1) != 0) break;

                if (sideDist.x < sideDist.y && sideDist.x < sideDist.z) {
                    sideDist.x += abs(invDir.x) * scale;
                    stepIdx = 1;
                } else if (sideDist.y < sideDist.z) {
                    sideDist.y += abs(invDir.y) * scale;
                    stepIdx = 16;
                } else {
                    sideDist.z += abs(invDir.z) * scale;
                    stepIdx = 4;
                }
                // Step position in selected axis, same as:
                //   currIdx += (dir[stepAxis] < 0 ? -stepIdx : +stepIdx)
                int prevIdx = currIdx;
                currIdx += (stepIdx ^ signMask) - signMask;

                // Bounds check: stop if increment flips any bit outside axis mask
                if (uint(prevIdx ^ currIdx) >= uint(stepIdx << 2)) { currIdx = -1; break; }
            }

            // Descend if we hit any child
            if (currIdx >= 0) {
                // Undo last step
                if (stepIdx == 1) sideDist.x -= abs(invDir.x) * scale;
                if (stepIdx == 4) sideDist.z -= abs(invDir.z) * scale;
                if (stepIdx == 16) sideDist.y -= abs(invDir.y) * scale;
                
                if (stepIdx == 0) {
                    // This is the first iteration and origin is directly over a child - don't step, just descend
                    sideDist.x = 0;
                }
                // Compute hit pos
                tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
                float3 hitPos = origin + tmin * dir;
                
                if (node->IsLeaf) break;
                
                // Clamp hit pos to be inside child bounding box to workaround float precision errors
                uint3 childPos = currIdx >> uint3(0, 4, 2) & 3u;
                childPos = (asuint(pos) & (~3u << scale_exp)) | childPos << scale_exp;
                
                int child_scale_exp = scale_exp - 2;
                uint3 subPos = clamp(asuint(hitPos), childPos, childPos | (3u << child_scale_exp));
                subPos &= ~0u << child_scale_exp; // floor
                
                pos = asfloat(subPos);
                
                stack[scale_exp >> 1] = uint(uint64_t(node) - uint64_t(TreeNodes)); // push
                scale_exp = child_scale_exp;
                
                node = &TreeNodes[node->ChildPtr + popcnt_var64(node->PopMask, currIdx)];
            } else {
                uint3 parentPos = asuint(pos);
                
                // Compute step position to figure out the parent scale. For example,
                // if we are at scale 1/64 and step crosses boundary of node 1/4, climb directly to it.
                int stepDelta = (signMask & stepIdx) != 0 ? -1 : +1;
                int stepPos;
                if (stepIdx == 1) stepPos = parentPos.x;
                if (stepIdx == 4) stepPos = parentPos.z;
                if (stepIdx == 16) stepPos = parentPos.y;
                
                int parent_scale_exp = scale_exp + 2;
                scale_exp = firstbithigh(stepPos ^ (stepPos + (stepDelta << parent_scale_exp))); // 31 - lzcnt
                if (scale_exp % 2 == 0) scale_exp--;  // dunno why diff gets misaligned, but this seems to work alright

                if (scale_exp > 21) break;
                
                // Step again using actual scale
                parentPos &= ~0u << scale_exp;
                
                stepDelta <<= scale_exp;
                if (stepIdx == 1) parentPos.x += stepDelta;
                if (stepIdx == 4) parentPos.z += stepDelta;
                if (stepIdx == 16) parentPos.y += stepDelta;

                pos = asfloat(parentPos);

                node = (Node*)(uint64_t(TreeNodes) + stack[scale_exp >> 1]); // pop
            }

        }
        PERF_STAT_INC(TraversalIters, j);
        
        HitInfo hit;
        hit.MaterialId = 0;
        
        if (node->IsLeaf) {
            hit.MaterialId = LeafData[node->ChildPtr + popcnt_var64(node->PopMask, currIdx)];
            hit.Pos = origin + tmin * dir;

            bool3 sideMask = stepIdx == int3(1, 16, 4);
            hit.Normal = select(sideMask, -sign(dir), 0.0);
            return hit;
        }
        return hit;
    }

    static int GetNodeCellIndex(float3 pos, int scale_exp) {
        uint3 cellPos = asuint(pos) >> scale_exp & 3;
        return cellPos.x + cellPos.z * 4 + cellPos.y * 16;
    }
    // floor(pos / scale) * scale
    static float3 FloorScale(float3 pos, int scale_exp) {
        uint mask = ~0u << scale_exp;
        return asfloat(asuint(pos) & mask);
    }
}



};
