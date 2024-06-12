import GBuffer;
import DenoiseCommon;

struct DispatchParams {
    GBufferUniforms* GBuffer;
    bool ForceResetHistory;
    uint padding;
};
[vk::push_constant] DispatchParams pc;

[numthreads(8, 8, 1)]
void ComputeMain(int2 pos: SV_DispatchThreadID) {
    if (!Reproject(pos)) {
        pc.GBuffer->HistoryLenTex.Instance[pos] = 0;
        pc.GBuffer->MomentsTex.Instance[pos] = float2(0);
    }
}

bool Reproject(int2 pos) {
    float depth = pc.GBuffer->DepthTex.Instance[pos];
    if (depth <= 0) return false;  // sky

    float3 worldPos = GetWorldPos(pos, depth, pc.GBuffer->InvProjMat);

    float2 prevPos = GetHistoryPos(worldPos);
    int2 prevPosi = int2(prevPos);
    float2 prevPosf = fract(prevPos);

    if (!pc.GBuffer->CheckBounds(prevPosi)) return false;
    // if (prevPosi == pos && dot(prevPosf, prevPosf) < 0.025*0.025) return true;

    float3 currAlbedo = pc.GBuffer->AlbedoTex.Instance[pos].rgb;
    float3 currNormal = UnpackGNormal(pc.GBuffer->AlbedoTex.Instance[pos].w);

    float wsum = 0.0;
    float3 prevIrradiance = float3(0);
    float2 prevMoments = float2(0);
    uint historyLen = pc.GBuffer->HistoryLenTex.Instance[pos];

    for (int i = 0; i < 4; i++) {
        int2 samplePos = prevPosi + int2(i & 1, i >> 1);
        if (!IsValidSample(samplePos, worldPos, currNormal)) continue;

        float w = ((i & 1) != 0 ? prevPosf.x : 1.0 - prevPosf.x) *
                  ((i >> 1) != 0 ? prevPosf.y : 1.0 - prevPosf.y);

        prevIrradiance += pc.GBuffer->PrevIrradianceTex.Instance[samplePos].rgb * w;
        prevMoments += pc.GBuffer->PrevMomentsTex.Instance[samplePos].rg * w;
        wsum += w;

        // This helps minimize smearing after disocclusion
        historyLen = min(historyLen, pc.GBuffer->HistoryLenTex.Instance[samplePos].r + 1);
    }
    if (wsum < 0.001) return false;

    prevIrradiance /= wsum;
    prevMoments /= wsum;

    if (pc.ForceResetHistory && historyLen > 6) historyLen = 6;
    float blendFactor = 1.0 / (historyLen + 1);

    float4 currIrradiance = pc.GBuffer->IrradianceTex.Instance[pos];
    float3 newIrradiance = lerp(prevIrradiance, currIrradiance.rgb, blendFactor);

    float luma = GetLuminance(newIrradiance);
    float2 currMoments = float2(luma, luma * luma);
    float2 newMoments = lerp(prevMoments, currMoments, max(0.5, blendFactor));
    pc.GBuffer->MomentsTex.Instance[pos] = newMoments;

    float variance = max(0.0, newMoments.y - newMoments.x * newMoments.x);
    pc.GBuffer->IrradianceTex.Instance[pos] = float4(newIrradiance, variance);

    historyLen = min(historyLen + 1, 64);
    pc.GBuffer->HistoryLenTex.Instance[pos] = uint8_t(historyLen);

    return true;
}

bool IsValidSample(int2 samplePos, float3 centerWorldPos, float3 centerNormal) {
    // Normal check
    float3 sampleNormal = UnpackGNormal(pc.GBuffer->PrevAlbedoTex.Instance[samplePos].w);
    if (dot(centerNormal, sampleNormal) < 0.5) return false;

    // Plane distance check (ReBLUR)
    float sampleDepth = pc.GBuffer->PrevDepthTex.Instance[samplePos].r;
    if (sampleDepth <= 0.0) return false;  // sky
    float3 sampleWorldPos = GetWorldPos(samplePos, sampleDepth, pc.GBuffer->HistoryInvProjMat);

    float planeDist = abs(dot(centerWorldPos - sampleWorldPos + pc.GBuffer->OriginDelta, centerNormal));
    if (planeDist > 6.0) return false;

    return true;
}

float3 GetWorldPos(int2 screenPos, float depth, float4x4 invProj) {
    float4 worldPos = mul(invProj, float4(screenPos, depth, 1.0));
    return worldPos.xyz * (16.0 / worldPos.w);
}
float2 GetHistoryPos(float3 worldPos) {
    float4 prevNDC = mul(pc.GBuffer->HistoryProjMat, float4(worldPos + pc.GBuffer->OriginDelta, 1.0));
    float2 prevUV = prevNDC.xy / prevNDC.w;
    prevUV = prevUV * 0.5 + 0.5;

    return prevUV * pc.GBuffer->RenderSize - 0.5;
}