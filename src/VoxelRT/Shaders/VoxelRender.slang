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

[vk::push_constant] DispatchParams pc;

[numthreads(8, 8)]
void ComputeMain(uint2 pos: SV_DispatchThreadID) {
    float3 rayDir, rayPos;
    GetPrimaryRay(pos, rayPos, rayDir);

    var blueNoise = BlueNoise(pc.StbnTexture.Instance, pos, pc.GBuffer->FrameNo);

    float3 albedo, irradiance, normal;
    float depth = -1.0;

    uint clockStart = getRealtimeClockLow();

    HitInfo hit;
    if (RayCast(pc.Map, pc.WorldOrigin, rayPos, rayDir, hit)) {
        albedo = hit.Material.Color;
        normal = hit.Normal;
        
        float4 projPos = mul(pc.GBuffer->ProjMat, float4(hit.Pos / 16.0, 1.0)); // scale down by 1/16 to minimize precision loss
        depth = projPos.z / projPos.w;
        
        float3 throughput = 1.0;
        irradiance = albedo * hit.Material.Emission;

        if (pc.MaxBounces == 0) irradiance = 1.0;

        // very non PBR parameters
        const float3 sunDir = normalize(float3(0.3,0.9,-0.28));
        const float sunIntensity = 6;
        const float3 sunColor = float3(1.0, 0.9, 0.7);
    
        HitInfo sunHit;
        if (pc.MaxBounces > 0) {
            if (!RayCastCoarse(pc.Map, pc.WorldOrigin, hit.Pos + hit.Normal * 0.01, sunDir, sunHit)) {
                irradiance += sunColor * sunIntensity;
            } else {
                throughput *= 0.5;
            }
        }
        
        for (int i = 0; i < pc.MaxBounces; i++) {
            rayPos = hit.Pos + hit.Normal * 0.01;

            //float fuzziness = getMaterialMetalFuzziness(hit.mat);
            //if (fuzziness < 1.0) {
            //    rayDir = normalize(reflect(rayDir, hit.normal) + (fuzziness * random_dir(u_FrameNo, i)));
            //    if (dot(rayDir, hit.normal) <= 0.0) break;
            //} else 
            rayDir = normalize(hit.Normal + GetSphereDir(blueNoise.GetSample()));

            if (!RayCastCoarse(pc.Map, pc.WorldOrigin, rayPos, rayDir, hit)) {
                irradiance += throughput * GetSkyColor(rayDir, false);
                break;
            }
            float emission = hit.Material.Emission;

            if (i < 2 && !RayCastCoarse(pc.Map, pc.WorldOrigin, hit.Pos + hit.Normal * 0.01, sunDir, sunHit)) {
                throughput *= sunColor * sunIntensity;
                emission += sunIntensity;
            }
            
            throughput *= hit.Material.Color;
            irradiance += throughput * emission;
        }
    } else {
        albedo = float3(1.0);
        irradiance = GetSkyColor(rayDir, true);
        depth = -1.0;
    }

    uint clockEnd = getRealtimeClockLow();
    float clockDiff = (clockEnd - clockStart) / (200.0 * (pc.MaxBounces + 1));

    pc.GBuffer->AlbedoTex.Instance[pos] = float4(albedo, PackGNormal(normal));
    pc.GBuffer->IrradianceTex.Instance[pos] = float4(irradiance, clockDiff);
    pc.GBuffer->DepthTex.Instance[pos] = depth;
}

void GetPrimaryRay(int2 screenPos, out float3 rayPos, out float3 rayDir) {
    float4 far = mul(pc.GBuffer->InvProjMat, float4(screenPos, 1, 1));
    rayPos = pc.GBuffer->OriginFrac;
    rayDir = normalize(far.xyz / far.w);
}

[vk::binding(0, 1)]
[SamplerDesc(MagFilter.Linear, MinFilter.Linear, MipFilter.Linear, WrapMode.Repeat)]
SamplerState g_LinearSampler;

float3 GetSkyColor(float3 dir, bool isPrimary) {
    // return float3(0, 0, 1);
    // For light bounces, sample smaller mips to maybe improve cache coherency.
    return pc.SkyTexture.Instance.SampleLevel(g_LinearSampler, dir, isPrimary ? 0 : 3);
}
