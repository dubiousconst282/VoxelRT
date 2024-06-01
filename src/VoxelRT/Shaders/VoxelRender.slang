import Havk.Core;
import VoxelMap;
import VoxelTraversal;
import GBuffer;
import Noise;

struct DispatchParams {
    VoxelMap Map;
    GBufferUniforms* GBuffer;
    int3 WorldOrigin;

    uint MaxBounces;
    TextureHandle2D<uint2> StbnTexture;
    TextureHandleCube<float3> SkyTexture;
};

[vk::push_constant] ConstantBuffer<DispatchParams> pc;

[numthreads(8, 8)]
void ComputeMain(uint2 pos: SV_DispatchThreadID) {
    float3 rayDir, rayPos;
    GetPrimaryRay(pos, rayPos, rayDir);
    
    float3 albedo, irradiance, normal;
    float depth = -1.0;

    HitInfo hit;
    if (RayCast(pc.Map, pc.WorldOrigin, rayPos, rayDir, hit)) {
        albedo = hit.Material.Color;
        normal = hit.Normal;
        irradiance = float3(1.0);
    }

    // let blueNoise = BlueNoise(pc.StbnTexture.Instance, pc.GBuffer->FrameNo);

    pc.GBuffer->AlbedoTex.Instance[pos] = float4(albedo, PackGNormal(normal));
    pc.GBuffer->IrradianceTex.Instance[pos] = float4(irradiance, hit.Iters);
    pc.GBuffer->DepthTex.Instance[pos] = depth;
}

void GetPrimaryRay(int2 screenPos, out float3 rayPos, out float3 rayDir) {
    float4 far = mul(pc.GBuffer->InvProjMat, float4(screenPos, 1, 1));
    rayPos = pc.GBuffer->OriginFrac;
    rayDir = normalize(far.xyz / far.w);
}
