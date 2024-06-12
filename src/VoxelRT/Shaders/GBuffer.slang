import Havk.Core;

module GBuffer;

public struct GBufferUniforms {
    public ImageHandle2D<float4> AlbedoTex, PrevAlbedoTex;          // rgb: albedo, w: packed normal
    public ImageHandle2D<float4> IrradianceTex, PrevIrradianceTex;  // rgb: irradiance, w: variance
    public ImageHandle2D<float> DepthTex, PrevDepthTex;
    public ImageHandle2D<float2> MomentsTex, PrevMomentsTex;
    public ImageHandle2D<uint> HistoryLenTex;

    public float4x4 ProjMat, InvProjMat;
    public float4x4 HistoryProjMat, HistoryInvProjMat;
    public float3 OriginFrac, HistoryOriginFrac;
    public float3 OriginDelta;
    public uint FrameNo;

    public property uint2 RenderSize {
        [__ref]
        get {
            uint width, height;
            AlbedoTex.Instance.GetDimensions(width, height);
            return uint2(width, height);
        }
    }

    [__ref]
    public bool CheckBounds(int2 pos) {
        return all(uint2(pos) < RenderSize);
    }
};

public static float3 UnpackGNormal(float value) {
    return float3(uint3(value * 255 + 0.5) >> uint3(0, 2, 4) & 3u) - 1;
}
public static float PackGNormal(float3 normal) {
    uint3 inorm = uint3(clamp(normal + 1, 0, 3));
    return float(inorm.x << 0 | inorm.y << 2 | inorm.z << 4) / 255.0;
}