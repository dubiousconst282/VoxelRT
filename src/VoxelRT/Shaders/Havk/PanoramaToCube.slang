import Core;

#define PI 3.141592653589793
#define TAU 6.283185307179586

struct DispatchParams {
    float3* SourceImage; // RGB32
    uint2 SourceImageSize;
    ImageHandleArray2D<float3> DestCube;
    uint padding;
};
[vk::push_constant] DispatchParams pc;

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

float3 FetchLerpRow(int2 pos, float fx) {
    uint offset = pos.y * pc.SourceImageSize.x;

    float3 a = pc.SourceImage[offset + pos.x];
    float3 b = pc.SourceImage[offset + (pos.x + 1 >= pc.SourceImageSize.x ? 0 : pos.x + 1)];

    return lerp(a, b, fx);
}
float3 GetSourceSample(float2 uv) {
    int2 ipos = int2(floor(uv * pc.SourceImageSize - 0.5));
    float2 fpos = fract(uv * pc.SourceImageSize - 0.5);

    float3 rowA = FetchLerpRow(ipos, fpos.x);

    ipos.y = (ipos.y + 1 >= pc.SourceImageSize.y ? 0 : ipos.y + 1);
    float3 rowB = FetchLerpRow(ipos, fpos.x);

    return lerp(rowA, rowB, fpos.y);
}

[numthreads(8, 8, 1)]
void ComputeMain(uint3 pos: SV_DispatchThreadID) {
    float2 faceUV = float2(pos.xy) / pc.DestCube.Instance.Size.xy;
    float3 dir = normalize(UnprojectCubemap(faceUV, pos.z));
    float2 sourceUV = float2(atan2(dir.z, dir.x) / TAU, asin(dir.y) / PI) + 0.5;

    float3 color = GetSourceSample(sourceUV);
    pc.DestCube.Instance[pos] = color;
}