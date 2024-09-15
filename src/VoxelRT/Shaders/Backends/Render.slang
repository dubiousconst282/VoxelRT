import Havk.Core;
import BackendCommon;
import GBuffer;
import Utilities;

#include "PerfCounters.h.slang"

#ifndef BACKEND_ID
    #define BACKEND_ID 1
#endif

#if BACKEND_ID == 1
    import XBrickMap.Map;
    typealias TVoxelMap = backend::xbrickmap::VoxelMap;
#elif BACKEND_ID == 2
    import FlatDDA.Map;
    typealias TVoxelMap = backend::flat_dda::VoxelMap;
#elif BACKEND_ID == 3
    import FlatDDA.Map;
    typealias TVoxelMap = backend::flat_dda::VoxelMap_MultiDDA;
#elif BACKEND_ID == 4
    import DistField.Map;
    typealias TVoxelMap = backend::distfield::VoxelMap<1>;
#elif BACKEND_ID == 5
    import DistField.Map;
    typealias TVoxelMap = backend::distfield::VoxelMap<2>;
#elif BACKEND_ID == 6
    import OctantDF.Map;
    typealias TVoxelMap = backend::octdf::VoxelMap;
#elif BACKEND_ID == 7
    import ESVO.Map;
    typealias TVoxelMap = backend::esvo::VoxelMap;
#elif BACKEND_ID == 8
    import Tree64.Map;
    typealias TVoxelMap = backend::tree64::VoxelMap;
#elif BACKEND_ID == 9
    import BVH.Map;
    typealias TVoxelMap = backend::bvh::VoxelMap;
#else
    static_assert(false, "Unknown renderer backend ID");
#endif

struct DispatchParams {
    TVoxelMap Map;
    GBufferUniforms* GBuffer;
    
    uint MaxBounces;
    TextureHandle2D<uint2> StbnTexture;
    TextureHandleCube<float3> SkyTexture;

    Material* Palette;
    PerfStats* PerfCounters;
};

[vk::push_constant] DispatchParams pc;

[numthreads(8, 8)]
void ComputeMain(uint2 pos: SV_DispatchThreadID) {
    g_PerfTracker = pc.PerfCounters;

    uint64_t clockStart = clockARB();

    float3 rayDir, rayPos;
    GetPrimaryRay(pos, rayPos, rayDir);

    var blueNoise = BlueNoise(pc.StbnTexture.Instance, pos, pc.GBuffer->FrameNo);

    float3 irradiance = pc.MaxBounces == 0 ? 1.0 : 0.0;
    float3 throughput = 1.0;

    // very non PBR parameters
    const float3 sunDir = normalize(float3(0.3,0.9,-0.28));
    const float sunIntensity = 12;
    const float3 sunColor = float3(1.0, 0.9, 0.7);

    for (int bounceNo = 0; bounceNo <= pc.MaxBounces; bounceNo++) {
        PERF_STAT_INC(RayCasts, 1);
        PERF_TIME_BEGIN(ClocksPerRay);
        HitInfo hit = pc.Map.RayCast(pc.GBuffer->WorldOrigin, rayPos, rayDir, bounceNo != 0);
        PERF_TIME_END(ClocksPerRay);

        float3 albedo;
        float emission;

        if (!hit.Miss) {
            let material = pc.Palette[hit.MaterialId];
            albedo = material.Color;
            emission = material.Emission;
        }

        if (bounceNo == 0) {
            if (hit.Miss) {
                pc.GBuffer->AlbedoTex.Instance[pos] = float4(1.0);
                irradiance = GetSkyColor(rayDir, true);
                pc.GBuffer->DepthTex.Instance[pos] = -1.0;
                break;
            }
            if (pc.MaxBounces == 0) {
                // fake lighting just so that faces are distinguishable
                irradiance *= hit.Normal.x != 0 ? 0.8 : 
                              hit.Normal.z != 0 ? 0.5 : 
                              hit.Normal.y  < 0 ? 0.3 : 1.0;
            }
            pc.GBuffer->AlbedoTex.Instance[pos] = PackAlbedo(albedo, hit.Normal, emission > 0.5);

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

    uint64_t clockEnd = clockARB();
    float clockDiff = (clockEnd - clockStart) / 1000.0;

    #if PERF_PRECISE_ITER_HEATMAP
    clockDiff = g_CurrTraversalIters;
    #endif
    
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
