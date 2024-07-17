import Havk.Core;
import VoxelMap;
import VoxelTraversal;
import GBuffer;
import Utilities;

struct DispatchParams {
    VoxelMap Map;
    GBufferUniforms* GBuffer;

    uint MaxBounces;
    TextureHandle2D<uint2> StbnTexture;
    TextureHandleCube<float3> SkyTexture;
};

[vk::push_constant] DispatchParams pc;

[numthreads(8, 8)]
void ComputeMain(uint2 pos: SV_DispatchThreadID) {
    uint clockStart = getRealtimeClockLow();

    float3 rayDir, rayPos;
    GetPrimaryRay(pos, rayPos, rayDir);

    var blueNoise = BlueNoise(pc.StbnTexture.Instance, pos, pc.GBuffer->FrameNo);

    float3 irradiance = pc.MaxBounces == 0 ? 1.0 : 0.0;
    float3 throughput = 1.0;

    for (int bounceNo = 0; bounceNo <= pc.MaxBounces; bounceNo++) {
        HitInfo hit = RayCast(pc.Map, pc.GBuffer->WorldOrigin, rayPos, rayDir, bounceNo != 0);
        float3 albedo;
        float emission;

        if (!hit.Miss) {
            let material = pc.Map.GetMaterialFromId(hit.MaterialId);
            albedo = material.Color;
            emission = material.Emission;

            // very non PBR parameters
            const float3 sunDir = normalize(float3(0.3,0.9,-0.28));
            const float sunIntensity = 12;
            const float3 sunColor = float3(1.0, 0.9, 0.7);
            
            if (false&&pc.MaxBounces > 0) {
                if (dot(hit.Normal, sunDir) > 0 &&  // trivial filter
                    RayCast(pc.Map, pc.GBuffer->WorldOrigin, hit.Pos + hit.Normal * 0.01, sunDir, true).Miss
                ) {
                    throughput *= sunColor;
                    emission += sunIntensity;
                } else {
                    throughput *= 0.7;
                }
            }
        }

        if (bounceNo == 0) {
            if (hit.Miss) {
                pc.GBuffer->AlbedoTex.Instance[pos] = float4(1.0);
                irradiance = GetSkyColor(rayDir, true);
                pc.GBuffer->DepthTex.Instance[pos] = -1.0;
                break;
            }
            pc.GBuffer->AlbedoTex.Instance[pos] = float4(albedo, PackGNormal(hit.Normal));

            float4 projPos = mul(pc.GBuffer->ProjMat, float4(hit.Pos / 16.0, 1.0)); // scale down by 1/16 to minimize precision loss
            float depth = projPos.z / projPos.w;
            pc.GBuffer->DepthTex.Instance[pos] = depth;
        } else {
            if (hit.Miss) {
                irradiance += GetSkyColor(rayDir, false) * throughput;
                break;
            }
            throughput *= albedo;
        }
        
        irradiance += throughput * emission;
        
        //float fuzziness = getMaterialMetalFuzziness(hit.mat);
        //if (fuzziness < 1.0) {
        //    rayDir = normalize(reflect(rayDir, hit.normal) + (fuzziness * random_dir(u_FrameNo, i)));
        //    if (dot(rayDir, hit.normal) <= 0.0) break;
        //} else 
        rayDir = normalize(hit.Normal + GetSphereDir(blueNoise.GetSample()));
        rayPos = hit.Pos + hit.Normal * 0.01;
    }

    uint clockEnd = getRealtimeClockLow();
    float clockDiff = (clockEnd - clockStart) / (3000.0 * (pc.MaxBounces + 1));

    pc.GBuffer->IrradianceTex.Instance[pos] = float4(irradiance, clockDiff);
}

void GetPrimaryRay(int2 screenPos, out float3 rayPos, out float3 rayDir) {
    float2 pos = float2(screenPos) + 0.5;
    if (pc.GBuffer->NumSteadyFrames > 0) {
        float easeWeight = max(1, 10 - pc.GBuffer->NumSteadyFrames);
        pos += (MartinR2(pc.GBuffer->NumSteadyFrames)  - 0.5)/ easeWeight;
    }
    float4 far = mul(pc.GBuffer->InvProjMat, float4(pos, 1, 1));
    rayPos = pc.GBuffer->OriginFrac;
    rayDir = normalize(far.xyz / far.w);
}

[vk::binding(0, 1)]
[SamplerDesc(MagFilter.Linear, MinFilter.Linear, MipFilter.Linear, WrapMode.Repeat)]
SamplerState g_LinearSampler;

float3 GetSkyColor(float3 dir, bool isPrimary) {
    // return float3(0, 0, 1);
    // For light bounces, sample smaller mips to maybe improve cache coherency.
    return pc.SkyTexture.Instance.SampleLevel(g_LinearSampler, dir, isPrimary ? 0 : 3) * (isPrimary?1:4);
}
