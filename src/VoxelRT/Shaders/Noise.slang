public struct Random {
    uint _state;

    public __init(uint3 p) {
        uint2 q = 1103515245U * p.xy;
        uint n = q.x ^ (q.y >> 3U);
        _state = n + p.z * 12345u;
    }
    public __init(float2 uv, uint time) {
        _state = bit_cast<uint>(uv.x);
        _state = RandomU32() ^ bit_cast<uint>(uv.y) ^ time;
    }

    // https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
    [__ref]
    public uint RandomU32() {
        uint state = _state;
        _state = _state * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }
    [__ref]
    public float Random() {
        return float(RandomU32()) * (1.0 / 4294967296.0);
    }
};

public struct BlueNoise {
    Texture2D<uint2> stbnTexture;
    uint noiseTileY, sampleIdx;

    public __init(Texture2D<uint2> stbn, uint frameNo) {
        stbnTexture = stbn;
        noiseTileY = (frameNo & 63) * 128;
        sampleIdx = 0;
    }

    // Returns 2 blue noise samples
    // https://developer.nvidia.com/blog/rendering-in-real-time-with-spatiotemporal-blue-noise-textures-part-1/
    [__ref]
    public float2 GetSample(int2 pos) {
        // R2 quasirandom sequence
        float2 sampleOffset = fract(sampleIdx * float2(0.75487766624669276005, 0.56984029099805326591) + 0.5);
        sampleIdx++;

        pos = (pos + int2(sampleOffset * 128)) & 127;
        pos.y += noiseTileY;

        return (stbnTexture[pos] + 0.5) / 256.0;
    }
};

public static float3 RandomSphereDir(float2 sample) {
    float y = sample.x * 2 - 1;
    float a = sample.y * 6.283185307179586;

    float sy = sqrt(1.0 - y * y);
    return float3(sin(a) * sy, y, cos(a) * sy);
}