import Havk.Core;

const float PI = 3.141592653589793;
const float TAU = 6.283185307179586;

struct DispatchParams {
    TextureHandle2D SourceImage;
    ImageHandle<RWTexture2DArray<float3>> DestCube;
};
[vk::push_constant] ConstantBuffer<DispatchParams> pc;

[SamplerDesc(MagFilter.Linear, MinFilter.Linear, MipFilter.Linear, WrapMode.Repeat, 0.0f, CompareOp.None)]
SamplerState g_LinearSampler;

float3 UnprojectCubemap(float2 uv, uint faceIdx) {
    float uc = uv.x * 2.0 - 1.0;
    float vc = uv.y * 2.0 - 1.0;

    switch (faceIdx) {
        default:
        case 0: return float3(+1.0,   vc,  -uc); // POSITIVE X
        case 1: return float3(-1.0,   vc,   uc); // NEGATIVE X
        case 2: return float3(  uc, -1.0,   vc); // POSITIVE Y
        case 3: return float3(  uc, +1.0,  -vc); // NEGATIVE Y
        case 4: return float3(  uc,   vc, +1.0); // POSITIVE Z
        case 5: return float3( -uc,   vc, -1.0); // NEGATIVE Z
    }
}

[numthreads(8, 8, 1)]
void ComputeMain(uint3 pos: SV_DispatchThreadID) {
    float2 faceUV = float2(pos.xy) / pc.DestCube.Instance.Size.xy;
    float3 dir = normalize(UnprojectCubemap(faceUV, pos.z));
    float2 sourceUV = float2(atan2(dir.z, dir.x) / TAU, asin(dir.y) / PI) + 0.5;

    float4 color = pc.SourceImage.Instance.Sample(g_LinearSampler, sourceUV);
    pc.DestCube.Instance[pos] = color.rgb;
}