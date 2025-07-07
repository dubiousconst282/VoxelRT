import "../BackendCommon";

struct StorageData {
    Material Palette[256];
    uint64_t QuadData[];
};
struct DispatchParams {
    StorageData* Storage;
    int3 WorldOrigin;
    float4x4 ViewProjMat;
    bool EnableTriangleColoring;
};

[vk::push_constant] DispatchParams pc;

struct ShadedVertex {
    float4 FragPos : SV_Position;
    float3 WorldPos;
    nointerpolation float3 Normal;
    nointerpolation float3 Color;
};

static const float3 g_NormalLookup[6] = {
    float3( 0, 1, 0),
    float3( 0,-1, 0),
    float3( 1, 0, 0),
    float3(-1, 0, 0),
    float3( 0, 0, 1),
    float3( 0, 0,-1)
};
static const int g_FlipLookup[6] = { 1, -1, -1, 1, -1, 1 };

[shader("vertex")]
ShadedVertex VertexMain(uint vertexId: SV_VertexID, uint baseVertex: SV_StartVertexLocation, uint instanceId: SV_StartInstanceLocation) {
    vertexId += baseVertex;

    int3 chunkOffsetPos = (instanceId >> int3(0, 8, 16) & 255) * 62;
    uint face = instanceId >> 24;

    int cornerId = (vertexId & 3);
    uint64_t quadData = pc.Storage->QuadData[vertexId >> 2];

    int3 quadPos = int(quadData) >> int3(0, 6, 12) & 63;
    int2 quadSize = int(quadData) >> int2(18, 24) & 63;
    uint wDir = (face & 2) >> 1, hDir = 2 - (face >> 2);
    int wMod = cornerId >> 1, hMod = cornerId & 1;

    quadPos += chunkOffsetPos;
    quadPos[wDir] += (quadSize.x * wMod * g_FlipLookup[face]);
    quadPos[hDir] += (quadSize.y * hMod);

    ShadedVertex vtx;
    vtx.Normal = g_NormalLookup[face];
    vtx.Color = pc.Storage->Palette[int(quadData >> 32) & 255].Color;

    float3 vertexPos = quadPos - pc.WorldOrigin;

    vtx.WorldPos = vertexPos;
    vtx.FragPos = mul(pc.ViewProjMat, float4(vertexPos, 1.0));

    return vtx;
}

[shader("fragment")]
float4 FragmentMain(ShadedVertex vtx, uint prim : SV_PrimitiveID) {
    const float3 diffuse_color = float3(0.15, 0.15, 0.15);
    const float3 rim_color = float3(0.04, 0.04, 0.04);
    const float3 sun_position = float3(250.0, 1000.0, 750.0) * 10000;
    const float3 eye_position = float3(0);

    float3 L = normalize(sun_position - vtx.WorldPos);
    float3 V = normalize(eye_position - vtx.WorldPos);

    float rim = 1.0 - max(dot(V, vtx.Normal), 0.0);
    rim = smoothstep(0.6, 1.0, rim);

    float3 color = vtx.Color;
    color += diffuse_color * max(0, dot(L, vtx.Normal));
    color += rim_color * rim;

    if (pc.EnableTriangleColoring) {
        color = fract(float3(0.123, 0.456, 0.789) * prim);
    }

    return float4(color, 1.0);
}