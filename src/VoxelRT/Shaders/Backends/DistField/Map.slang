import "../BackendCommon";
import "../ParametricTraversal";
import Havk.Core;

namespace backend::distfield {

public static const int 
    GRID_SIZE_XZ = 1024, GRID_SIZE_Y = 1024,
    TILE_SIZE = 128,
    TILE_STRIDE = TILE_SIZE*TILE_SIZE*TILE_SIZE;

public struct VoxelMap<let Metric = 1> : IVoxelMap, ITraversalStepper {
    public ImageHandle3D<uint> DistField;
    
    public static const uint2 GridSize = uint2(GRID_SIZE_XZ, GRID_SIZE_Y);

    public uint GetVoxelId(int3 pos) {
        return 1;
    }
    public Material GetMaterialFromId(uint materialId) {
        return materialId != 0 ? Material.DefaultOpaque : Material.DefaultEmpty;
    }

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        return ParametricRayCast(this, worldOrigin, rayPos, rayDir, coarse);
    }

    
    public bool GetStepPos(inout int3 pos, float3 dir, bool coarse) {
        uint distToNearest = DistField.Instance[pos];

        if (distToNearest <= 0) {
            return false;
        }

        int3 stepSize;

        if (Metric == 1) {
            // Manhattan
            float mag = 1.0 / (abs(dir.x) + abs(dir.y) + abs(dir.z)); // manhattan space normalization factor
            stepSize = int3(ceil(distToNearest * mag * dir - step(0.0, dir)));
        } else {
            // Euclidean
            stepSize = int3(sqrt(distToNearest) * dir);
        }

        // Advance ray, clipping to tile boundaries
        int3 stepPos = pos + stepSize;
        bool crossesTile = any(((pos ^ stepPos) & ~(TILE_SIZE - 1)) != 0);
        pos = crossesTile ? AlignToCellBoundaries(pos, dir, TILE_SIZE) : stepPos;

        // following is supposedly correct, but seems to miscompile in Mesa
        // bool crossesTile = any(uint3(pos ^ stepPos) >= TILE_SIZE);
        return true;
    }
}

};