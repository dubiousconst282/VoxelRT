import BackendCommon;

#include "PerfCounters.h.slang"

public interface ITraversalStepper {
    static const uint2 GridSize;

    bool GetStepPos(inout int3 pos, float3 dir, bool coarse);
    uint GetVoxelId(int3 pos);
}

// Sparse voxel ray-marching using the slab method to efficiently step
// into arbitrary voxels along the ray.
//
// This by itself is not much more expansive than DDA, but this naive  
// implementation is far more prone to float precision issues, which
// are currently mitigated by biasing the intersection distances.
public HitInfo SparseRayMarch<TMap : ITraversalStepper>(TMap map, int3 worldOrigin, float3 rayOrigin, float3 rayDir, bool coarse) {
    float3 invDir = 1.0 / rayDir;
    float3 tStart = (step(0.0, rayDir) - rayOrigin) * invDir;
    
    // Ray sometimes get stuck when origin is at exact integer coords.
    // This hack doesn't seem to always work though.
    tStart += select(abs(tStart) < 0.001, 0.001, 0);

    float3 startPos = ClipRayToAABB(rayOrigin, rayDir, -worldOrigin - 0.001, map.GridSize.xyx - worldOrigin + 0.001);
    int3 voxelPos = worldOrigin + int3(floor(startPos));

    float3 sideDist;
    int i = (coarse ? 80 : 256);
    int perf_NumIters = 0;
    
    while (--i >= 0) {
        perf_NumIters++;

        sideDist = tStart + float3(voxelPos - worldOrigin) * invDir;
        float tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
        tmin = coarse ? tmin + 0.001 : bit_cast<float>(bit_cast<uint>(tmin) + 5);
        float3 currPos = rayOrigin + tmin * rayDir;

        // Accurate stepping, helps when using small bias but alone isn't enough to prevent ray from getting stuck.
        //voxelPos = worldOrigin + ivec3(floor(mix(currPos, currPos + rayDir*0.01, sideMask)));
        voxelPos = worldOrigin + int3(floor(currPos));

        if (!CheckBounds(voxelPos, map.GridSize.x, map.GridSize.y)) { i = 0; }
        else if (!map.GetStepPos(voxelPos, rayDir, coarse && i < 60)) break;
    }
    PERF_STAT_INC(TraversalIters, perf_NumIters);

    HitInfo hit;
    hit.MaterialId = 0;
    
    // Calculate hit parameters outside the loop to minimize divergence and VGPR usage
    // (at least on Mesa this does make a difference)
    if (i >= 0) {
        hit.MaterialId = map.GetVoxelId(voxelPos);

        float tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
        float3 currPos = rayOrigin + tmin * rayDir;
        hit.Dist = tmin;
        hit.Pos = currPos;

        bool3 sideMask = float3(tmin) >= sideDist;
        hit.FaceUV = fract(select(sideMask.xz, currPos.yy, currPos.xz));
        hit.Normal = select(sideMask, -sign(rayDir), 0.0);
    }

    return hit;
}

public int3 AlignToCellBoundaries(int3 pos, float3 dir, int cellSize) {
    int cellMask = cellSize - 1;
    pos.x = dir.x < 0 ? (pos.x & ~cellMask) : (pos.x | cellMask);
    pos.y = dir.y < 0 ? (pos.y & ~cellMask) : (pos.y | cellMask);
    pos.z = dir.z < 0 ? (pos.z & ~cellMask) : (pos.z | cellMask);
    return pos;
}