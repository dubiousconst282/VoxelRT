
import "../BackendCommon";
#include "../PerfCounters.h.slang"

namespace backend::bvh {

public struct BvhNode {
    int16_t3 _bounds[2];
    uint32_t _childPtr;

    property bool IsLeaf {
        get { return (_childPtr & 1) != 0; }
    }
    property uint ChildPtr {
        get { return _childPtr >> 1; }
    }

    // invOriginT = -origin * invDir
    public Optional<float> Intersect(float3 invDir, float3 invOriginT) {
        float3 t0 = fma(_bounds[0], invDir, invOriginT);
        float3 t1 = fma(_bounds[1], invDir, invOriginT);

        float3 temp = t0;
        t0 = min(temp, t1);
        t1 = max(temp, t1);

        float tmin = max(max(t0.x, t0.y), t0.z);
        float tmax = min(min(t1.x, t1.y), t1.z);

        if (tmin < tmax && tmax > 0) return tmin;
        return none;
    }
}

public struct Brick {
    uint64_t PopMask[8];
}

public struct VoxelMap : IRayCastable {
    public BvhNode* Nodes;
    public Brick* Bricks;
    public uint RootIdx;

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        float3 origin = worldOrigin + rayPos;

        HitInfo hit = Traversal(origin, rayDir, coarse);
        
        if (hit.MaterialId != 0) {
            hit.Pos = 1;
        }
        return hit;
    }

    private HitInfo Traversal(float3 origin, float3 dir, bool coarse) {
        if (abs(dir.x) < 0.0001) dir.x = 0.0001;
        if (abs(dir.y) < 0.0001) dir.y = 0.0001;
        if (abs(dir.z) < 0.0001) dir.z = 0.0001;
        
        float3 invDir = 1.0 / dir;
        float3 invOriginT = -origin * invDir;

        uint stack[64];
        int stackPtr = 0;

        int currIdx = RootIdx;
        BvhNode node = Nodes[currIdx];
        
        float tmin;
        int i = 0;
        const int maxIters = 128;
        
        for (; i < maxIters; i++) {
            for (; i < maxIters && !node.IsLeaf; i++) {
                uint childIdxA = node.ChildPtr + 0;
                uint childIdxB = node.ChildPtr + 1;

                Optional<float> timeA = Nodes[childIdxA].Intersect(invDir, invOriginT);
                Optional<float> timeB = Nodes[childIdxB].Intersect(invDir, invOriginT);

                if (timeA.hasValue) {
                    if (timeB.hasValue) {
                        if (timeA.value > timeB.value) {
                            uint temp = childIdxA;
                            childIdxA = childIdxB;
                            childIdxB = temp;
                        }
                        stack[stackPtr++] = childIdxB;
                    }
                    currIdx = childIdxA;
                } else if (timeB.hasValue) {
                    currIdx = childIdxB;
                } else {
                    if (stackPtr == 0) break;
                    currIdx = stack[--stackPtr];
                }
                node._childPtr = Nodes[currIdx]._childPtr;
            }
            if (stackPtr == 0) break;

            if (node.IsLeaf) {
                node._bounds = Nodes[currIdx]._bounds; // fetch bounds only when needed

                var dist = node.Intersect(invDir, invOriginT);
                tmin = dist.hasValue ? dist.value : 0;

                int3 pos = int3(floor(origin + tmin * dir));
                pos = clamp(pos, node._bounds[0], node._bounds[1] - 1);
        
                float3 sideDist = (step(0.0f, dir) + pos) * invDir + invOriginT;

                if (TraverseBrick(node, pos & 7, invDir, sideDist, tmin))
                    break;

                if (stackPtr == 0) break;
                currIdx = stack[--stackPtr];
                node._childPtr = Nodes[currIdx]._childPtr;
                continue;
            }
        }
        PERF_STAT_INC(TraversalIters, i);

        HitInfo hit;
        hit.MaterialId = 0;

        if (node.IsLeaf) {
            hit.MaterialId = kDefaultOpaqueMaterialId;
            hit.Dist = tmin;
            hit.Pos = origin + dir * tmin;
        }
        return hit;
    }


    private bool TraverseBrick(BvhNode node, int3 pos, float3 invDir, float3 sideDist, inout float tmin) {
        uint64_t* bitmap = &Bricks[node.ChildPtr].PopMask[0];
        uint64_t mask = bitmap[pos.y];

        while (uint(pos.x | pos.y | pos.z) < 8u) {
            #if PERF_PRECISE_ITER_HEATMAP
            g_CurrTraversalIters++;
            #endif

            tmin = min(min(sideDist.x, sideDist.y), sideDist.z);

            if ((mask >> (pos.x + pos.z * 8) & 1) != 0) {
                return true;
            }

            if (tmin == sideDist.x) {
                sideDist.x += abs(invDir.x);
                pos.x += (invDir.x < 0 ? -1 : +1);
            } else if (tmin == sideDist.y) {
                sideDist.y += abs(invDir.y);
                pos.y += (invDir.y < 0 ? -1 : +1);
                mask = bitmap[pos.y];
            } else {
                sideDist.z += abs(invDir.z);
                pos.z += (invDir.z < 0 ? -1 : +1);
            }
        }
        return false;
    }
}

};
