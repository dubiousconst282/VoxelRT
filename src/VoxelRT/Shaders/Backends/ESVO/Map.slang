import "../BackendCommon";
import Raycast;

namespace backend::esvo {

public struct VoxelMap : IVoxelMap {
    public uint* RootNode;
    public uint TreeScale;

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        Ray ray;
        ray.dir = rayDir;

        // Multiply world origin separately to minimize float rounding errors.
        // Compiler might mess with this, but oh well.
        float scale = 1.0 / (1 << TreeScale);
        ray.orig = worldOrigin * scale + rayPos * scale + 1.0;
        
        // TODO: figure these out
        ray.dir_sz = 0;
        ray.orig_sz = 0;
        
        CastResult result = CastRay(ray, RootNode);
        
        HitInfo hit;
        hit.MaterialId = 0;

        if (result.t <= 1.0) {
            hit.Pos = (result.pos - 1.0 - worldOrigin * scale) * (1 << TreeScale);
            hit.Normal = result.normal;
            hit.MaterialId = 1;
        }
        return hit;
    }

    public Material GetMaterialFromId(uint materialId) {
        return materialId != 0 ? Material.DefaultOpaque : Material.DefaultEmpty;
    }
}

};